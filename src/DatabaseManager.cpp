// src/DatabaseManager.cpp
#include "DatabaseManager.h"
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <thread>
#include <atomic>
#include <fstream>
#include <zlib.h>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <iomanip>
#include <cctype>
#include "RomScanner.h"

// Helper function to safely get text from SQLite column
static std::string safe_column_text(sqlite3_stmt* stmt, int column) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    return text ? std::string(text) : std::string("");
}

// Sanitize stored filesystem paths: remove control characters and trim
static std::string sanitize_path(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (unsigned char c : p) {
        if (c == '\r' || c == '\n' || c == '\t') continue;
        out.push_back(static_cast<char>(c));
    }
    // Trim leading/trailing spaces
    size_t start = 0;
    while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start]))) start++;
    size_t end = out.size();
    while (end > start && std::isspace(static_cast<unsigned char>(out[end-1]))) end--;
    return out.substr(start, end - start);
}

DatabaseManager::DatabaseManager(const std::string& db_path) : m_db(nullptr), m_db_path(db_path) {}

DatabaseManager::~DatabaseManager() {
    if (m_db) {
        sqlite3_close(m_db);
    }
}

bool DatabaseManager::initialize() {
    std::cerr << "[DEBUG] Opening database: " << m_db_path << std::endl;
    int rc = sqlite3_open(m_db_path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur ouverture base de données: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    std::cerr << "[DEBUG] Database opened successfully" << std::endl;

    // Performance + concurrency settings
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;",      nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA synchronous=NORMAL;",    nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA cache_size=-32768;",     nullptr, nullptr, nullptr); // 32 MB page cache
    sqlite3_exec(m_db, "PRAGMA temp_store=MEMORY;",     nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA foreign_keys=ON;",       nullptr, nullptr, nullptr);

    if (!createTables()) return false;

    // Repair any existing rom_cache rows with stray control characters in filepath
    const char* sel_sql = "SELECT id, filepath FROM rom_cache;";
    sqlite3_stmt* sel_stmt;
    if (sqlite3_prepare_v2(m_db, sel_sql, -1, &sel_stmt, nullptr) == SQLITE_OK) {
        const char* upd_sql = "UPDATE rom_cache SET filepath = ? WHERE id = ?;";
        sqlite3_stmt* upd_stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, upd_sql, -1, &upd_stmt, nullptr) != SQLITE_OK) upd_stmt = nullptr;
        while (sqlite3_step(sel_stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(sel_stmt, 0);
            std::string fp = safe_column_text(sel_stmt, 1);
            std::string san = sanitize_path(fp);
            if (san != fp) {
                if (upd_stmt) {
                    sqlite3_reset(upd_stmt);
                    sqlite3_bind_text(upd_stmt, 1, san.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(upd_stmt, 2, id);
                    sqlite3_step(upd_stmt);
                }
            }
        }
        if (upd_stmt) sqlite3_finalize(upd_stmt);
        sqlite3_finalize(sel_stmt);
    }

    return true;
}

bool DatabaseManager::createTables() {
    // Create tables only if they don't exist (preserve existing data)
    char* err_msg = nullptr;
    
    const char* create_games_sql = R"(
        CREATE TABLE IF NOT EXISTS games (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT,
            year TEXT,
            manufacturer TEXT,
            system TEXT,
            status TEXT DEFAULT 'missing',
            video_type TEXT,
            orientation TEXT,
            width TEXT,
            height TEXT,
            aspect_x TEXT,
            aspect_y TEXT,
            driver_status TEXT,
            comment TEXT,
            cloneof TEXT,
            romof TEXT,
            sourcefile TEXT,
            snapshot_path TEXT,
            dat_source TEXT,
            UNIQUE(name, system)
        );
    )";
    
    const char* create_roms_sql = R"(
        CREATE TABLE IF NOT EXISTS roms (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            game_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            size INTEGER,
            crc TEXT,
            file_path TEXT,
            FOREIGN KEY(game_id) REFERENCES games(id),
            UNIQUE(game_id, name)
        );
    )";
    
    const char* create_dat_files_sql = R"(
        CREATE TABLE IF NOT EXISTS dat_files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filename TEXT UNIQUE NOT NULL,
            filepath TEXT NOT NULL,
            last_modified INTEGER NOT NULL,
            file_size INTEGER NOT NULL,
            games_count INTEGER DEFAULT 0
        );
    )";

    const char* create_rom_cache_sql = R"(
        CREATE TABLE IF NOT EXISTS rom_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            rom_id INTEGER,
            filename TEXT NOT NULL,
            filepath TEXT NOT NULL,
            last_modified INTEGER NOT NULL,
            file_size INTEGER NOT NULL,
            last_scan_time INTEGER NOT NULL,
            dat_timestamp INTEGER NOT NULL,
            file_crc INTEGER DEFAULT 0,
            FOREIGN KEY(rom_id) REFERENCES roms(id) ON DELETE SET NULL
        );
    )";

    const char* create_scan_metadata_sql = R"(
        CREATE TABLE IF NOT EXISTS scan_metadata (
            key TEXT PRIMARY KEY,
            value INTEGER NOT NULL
        );
    )";

    const char* create_snapshots_sql = R"(
        CREATE TABLE IF NOT EXISTS directory_snapshots (
            path TEXT PRIMARY KEY,
            file_count INTEGER NOT NULL,
            last_modified INTEGER NOT NULL
        );
    )";

    const char* create_directory_files_sql = R"(
        CREATE TABLE IF NOT EXISTS directory_files (
            path TEXT NOT NULL,
            filename TEXT NOT NULL,
            file_size INTEGER NOT NULL,
            last_modified INTEGER NOT NULL,
            PRIMARY KEY(path, filename)
        );
    )";

    const char* create_saved_roots_sql = R"(
        CREATE TABLE IF NOT EXISTS saved_roots (
            path TEXT PRIMARY KEY
        );
    )";

    // Content-addressed ROM cache: the {entry_name, crc} contents of every scanned
    // ZIP (or loose file), keyed by canonical filepath. This lets us re-derive game
    // availability after a DAT update purely from cache, with no disk I/O.
    const char* create_zip_contents_sql = R"(
        CREATE TABLE IF NOT EXISTS zip_contents (
            filepath TEXT NOT NULL,
            entry_name TEXT NOT NULL,
            crc INTEGER NOT NULL,
            PRIMARY KEY(filepath, entry_name)
        );
    )";

    // Execute table creation
    
    if (sqlite3_exec(m_db, create_games_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table games: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    if (sqlite3_exec(m_db, create_roms_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table roms: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    if (sqlite3_exec(m_db, create_dat_files_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table dat_files: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_rom_cache_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table rom_cache: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    // Ensure `file_crc` column exists for rom_cache (add if missing)
    const char* pragma_sql = "PRAGMA table_info(rom_cache);";
    sqlite3_stmt* pragma_stmt;
    bool has_file_crc = false;
    if (sqlite3_prepare_v2(m_db, pragma_sql, -1, &pragma_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(pragma_stmt) == SQLITE_ROW) {
            std::string colname = safe_column_text(pragma_stmt, 1);
            if (colname == "file_crc") { has_file_crc = true; break; }
        }
        sqlite3_finalize(pragma_stmt);
    }

    if (!has_file_crc) {
        const char* alter_sql = "ALTER TABLE rom_cache ADD COLUMN file_crc INTEGER DEFAULT 0;";
        if (sqlite3_exec(m_db, alter_sql, 0, 0, &err_msg) != SQLITE_OK) {
            sqlite3_free(err_msg);
        }
    }

    // ── Migrate games table: add new columns if missing ─────────────────────
    struct ColDef { const char* name; const char* ddl; };
    static const ColDef new_cols[] = {
        { "is_favorite",      "ALTER TABLE games ADD COLUMN is_favorite INTEGER DEFAULT 0;" },
        { "last_played",      "ALTER TABLE games ADD COLUMN last_played TEXT DEFAULT NULL;" },
        { "play_count",       "ALTER TABLE games ADD COLUMN play_count INTEGER DEFAULT 0;" },
        { "play_time_secs",   "ALTER TABLE games ADD COLUMN play_time_secs INTEGER DEFAULT 0;" },
        { "source_directory", "ALTER TABLE games ADD COLUMN source_directory TEXT DEFAULT NULL;" },
    };
    {
        sqlite3_stmt* pi = nullptr;
        sqlite3_prepare_v2(m_db, "PRAGMA table_info(games);", -1, &pi, nullptr);
        std::set<std::string> existing_cols;
        while (sqlite3_step(pi) == SQLITE_ROW)
            existing_cols.insert(safe_column_text(pi, 1));
        sqlite3_finalize(pi);

        for (const auto& col : new_cols) {
            if (existing_cols.find(col.name) == existing_cols.end()) {
                if (sqlite3_exec(m_db, col.ddl, 0, 0, &err_msg) != SQLITE_OK) {
                    std::cerr << "[WARN] Could not add column " << col.name << ": " << err_msg << std::endl;
                    sqlite3_free(err_msg);
                }
            }
        }
    }

    if (sqlite3_exec(m_db, create_scan_metadata_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table scan_metadata: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_snapshots_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table directory_snapshots: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_directory_files_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table directory_files: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_saved_roots_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table saved_roots: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_zip_contents_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table zip_contents: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    // Index creation is best-effort and executed statement-by-statement: a missing
    // column on a legacy DB (e.g. an old rom_cache without rom_id) must neither abort
    // startup nor prevent the remaining indexes from being created. Indexes are pure
    // optimizations, so a failure here is a warning, not a fatal error.
    static const char* index_stmts[] = {
        "CREATE INDEX IF NOT EXISTS idx_games_name ON games(name);",
        "CREATE INDEX IF NOT EXISTS idx_games_system ON games(system);",
        "CREATE INDEX IF NOT EXISTS idx_games_status ON games(status);",
        "CREATE INDEX IF NOT EXISTS idx_roms_game_id ON roms(game_id);",
        "CREATE INDEX IF NOT EXISTS idx_dat_files_filename ON dat_files(filename);",
        "CREATE INDEX IF NOT EXISTS idx_rom_cache_filename ON rom_cache(filename);",
        "CREATE INDEX IF NOT EXISTS idx_rom_cache_rom_id ON rom_cache(rom_id);",
        "CREATE INDEX IF NOT EXISTS idx_zip_contents_crc ON zip_contents(crc);",
        "CREATE INDEX IF NOT EXISTS idx_zip_contents_path ON zip_contents(filepath);",
    };
    for (const char* idx_sql : index_stmts) {
        if (sqlite3_exec(m_db, idx_sql, 0, 0, &err_msg) != SQLITE_OK) {
            std::cerr << "[WARN] Index skipped (" << (err_msg ? err_msg : "?") << "): " << idx_sql << std::endl;
            sqlite3_free(err_msg);
            err_msg = nullptr;
        }
    }

    return true;
}

// Helper: compute CRC32 of a file (used for quick whole-file fingerprint)
static long compute_file_crc32(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        crc = crc32(crc, (const Bytef*)buffer, file.gcount());
    }
    return static_cast<long>(crc);
}

// Background cleanup thread data
static void cache_cleanup_thread_func(DatabaseManager* db, std::vector<std::string> rom_paths, std::atomic<bool>* running) {
    while (running->load()) {
        db->cleanupRomCache(rom_paths);
        for (int i = 0; i < 60 && running->load(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool DatabaseManager::insertGame(const Game& game) {
    const char* sql = R"(
        INSERT INTO games 
        (name, description, year, manufacturer, system, status, video_type, orientation, 
         width, height, aspect_x, aspect_y, driver_status, comment, cloneof, romof, sourcefile, snapshot_path, dat_source)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation requête insertGame: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, game.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game.description.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, game.year.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, game.manufacturer.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, game.system.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, game.status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, game.video_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, game.orientation.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, game.width.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, game.height.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, game.aspect_x.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 12, game.aspect_y.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, game.driver_status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 14, game.comment.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 15, game.cloneof.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 16, game.romof.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 17, game.sourcefile.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 18, "", -1, SQLITE_STATIC); // snapshot_path (empty for now)
    sqlite3_bind_text(stmt, 19, game.dat_source.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        std::cerr << "Erreur insertion game: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    // Get the inserted game's ID
    int64_t game_id = sqlite3_last_insert_rowid(m_db);
    
    // Insérer les ROMs
    for (const auto& rom : game.roms) {
        if (!insertRom(game_id, rom)) {
            return false;
        }
    }
    
    return true;
}

bool DatabaseManager::insertRom(int64_t game_id, const Rom& rom) {
    const char* sql = "INSERT OR REPLACE INTO roms (game_id, name, size, crc) VALUES (?, ?, ?, ?);";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation requête insertRom: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    sqlite3_bind_int64(stmt, 1, game_id);
    sqlite3_bind_text(stmt, 2, rom.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, rom.size);
    sqlite3_bind_text(stmt, 4, rom.crc.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateRomPath(const std::string& game_name, const std::string& rom_name, const std::string& file_path) {
    const char* sql = "UPDATE roms SET file_path = ? WHERE game_name = ? AND name = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rom_name.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameStatus(const std::string& game_name, const std::string& status) {
    const char* sql = "UPDATE games SET status = ? WHERE name = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameStatus(const std::string& game_name, const std::string& status, const std::string& system) {
    
    const char* sql = "UPDATE games SET status = ? WHERE name = ? AND system = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to prepare updateGameStatus: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_db);
    
    
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE && changes > 0) {
        // Force a sync to disk to ensure the change is persisted
        sqlite3_exec(m_db, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr);
    }
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameStatusWithSource(const std::string& game_name, const std::string& status, const std::string& system, const std::string& source_directory) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const char* sql = "UPDATE games SET status = ?, source_directory = ? WHERE name = ? AND system = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to prepare updateGameStatusWithSource: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    if (source_directory.empty())
        sqlite3_bind_null(stmt, 2);
    else
        sqlite3_bind_text(stmt, 2, source_directory.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, system.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool DatabaseManager::resetGamesFromDirectory(const std::string& directory) {
    const char* sql = "UPDATE games SET status = 'missing', source_directory = NULL WHERE source_directory = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to prepare resetGamesFromDirectory: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, directory.c_str(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);

    std::cerr << "[DEBUG] resetGamesFromDirectory(" << directory << "): reset " << changes << " games to missing" << std::endl;
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameSnapshot(const std::string& game_name, const std::string& snapshot_path) {
    const char* sql = "UPDATE games SET snapshot_path = ? WHERE name = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, snapshot_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::resetAllGamesToMissing() {
    const char* sql = "UPDATE games SET status = 'missing';";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to reset games to missing: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    // Force sync to disk
    sqlite3_exec(m_db, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr);
    
    return true;
}

std::vector<Game> DatabaseManager::getAllGames() {
    std::vector<Game> games;

    // Step 1: load every game in a single query, indexed by id for ROM stitching.
    const char* games_sql =
        "SELECT id, name, description, year, manufacturer, system, cloneof, romof, "
        "sourcefile, comment, video_type, orientation, width, height, aspect_x, "
        "aspect_y, driver_status, status, snapshot_path, dat_source "
        "FROM games ORDER BY description;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, games_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }

    std::unordered_map<int64_t, size_t> id_to_index;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Game game;
        int64_t game_id   = sqlite3_column_int64(stmt, 0);
        game.name         = safe_column_text(stmt, 1);
        game.description  = safe_column_text(stmt, 2);
        game.year         = safe_column_text(stmt, 3);
        game.manufacturer = safe_column_text(stmt, 4);
        game.system       = safe_column_text(stmt, 5);
        game.cloneof      = safe_column_text(stmt, 6);
        game.romof        = safe_column_text(stmt, 7);
        game.sourcefile   = safe_column_text(stmt, 8);
        game.comment      = safe_column_text(stmt, 9);
        game.video_type   = safe_column_text(stmt, 10);
        game.orientation  = safe_column_text(stmt, 11);
        game.width        = safe_column_text(stmt, 12);
        game.height       = safe_column_text(stmt, 13);
        game.aspect_x     = safe_column_text(stmt, 14);
        game.aspect_y     = safe_column_text(stmt, 15);
        game.driver_status= safe_column_text(stmt, 16);
        game.status       = safe_column_text(stmt, 17);
        game.snapshot_path= safe_column_text(stmt, 18);
        game.dat_source   = safe_column_text(stmt, 19);

        id_to_index[game_id] = games.size();
        games.push_back(std::move(game));
    }
    sqlite3_finalize(stmt);

    // Step 2: load every ROM in a single query and dispatch to its game.
    // Replaces the previous N+1 pattern (one prepare/step/finalize per game).
    const char* roms_sql = "SELECT game_id, name, size, crc FROM roms;";
    sqlite3_stmt* roms_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
            int64_t game_id = sqlite3_column_int64(roms_stmt, 0);
            auto it = id_to_index.find(game_id);
            if (it == id_to_index.end()) continue; // orphan row, skip

            Rom rom;
            rom.name = safe_column_text(roms_stmt, 1);
            rom.size = sqlite3_column_int64(roms_stmt, 2);
            rom.crc  = safe_column_text(roms_stmt, 3);
            games[it->second].roms.push_back(std::move(rom));
        }
        sqlite3_finalize(roms_stmt);
    }

    return games;
}

Game DatabaseManager::buildGameFromQuery(sqlite3_stmt* stmt) {
    // Column layout from SELECT * (col 0 = id):
    // 1:name 2:description 3:year 4:manufacturer 5:system 6:status
    // 7:video_type 8:orientation 9:width 10:height 11:aspect_x 12:aspect_y
    // 13:driver_status 14:comment 15:cloneof 16:romof 17:sourcefile
    // 18:snapshot_path 19:dat_source
    // 20:is_favorite 21:last_played 22:play_count 23:play_time_secs  (may be absent on old rows)
    Game game;
    game.name         = safe_column_text(stmt, 1);
    game.description  = safe_column_text(stmt, 2);
    game.year         = safe_column_text(stmt, 3);
    game.manufacturer = safe_column_text(stmt, 4);
    game.system       = safe_column_text(stmt, 5);
    game.status       = safe_column_text(stmt, 6);
    game.video_type   = safe_column_text(stmt, 7);
    game.orientation  = safe_column_text(stmt, 8);
    game.width        = safe_column_text(stmt, 9);
    game.height       = safe_column_text(stmt, 10);
    game.aspect_x     = safe_column_text(stmt, 11);
    game.aspect_y     = safe_column_text(stmt, 12);
    game.driver_status= safe_column_text(stmt, 13);
    game.comment      = safe_column_text(stmt, 14);
    game.cloneof      = safe_column_text(stmt, 15);
    game.romof        = safe_column_text(stmt, 16);
    game.sourcefile   = safe_column_text(stmt, 17);
    game.snapshot_path= safe_column_text(stmt, 18);
    game.dat_source   = safe_column_text(stmt, 19);

    int ncols = sqlite3_column_count(stmt);
    if (ncols > 20) game.is_favorite    = sqlite3_column_int(stmt, 20) != 0;
    if (ncols > 21) game.last_played    = safe_column_text(stmt, 21);
    if (ncols > 22) game.play_count     = sqlite3_column_int(stmt, 22);
    if (ncols > 23) game.play_time_secs = sqlite3_column_int(stmt, 23);
    return game;
}

std::vector<Game> DatabaseManager::getGamesBySystem(const std::string& system) {
    const char* sql = "SELECT * FROM games WHERE system = ? ORDER BY description;";
    
    sqlite3_stmt* stmt;
    std::vector<Game> games;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    
    sqlite3_bind_text(stmt, 1, system.c_str(), -1, SQLITE_STATIC);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        games.push_back(buildGameFromQuery(stmt));
    }
    
    sqlite3_finalize(stmt);
    return games;
}

std::vector<Game> DatabaseManager::getGamesByStatus(const std::string& status) {
    const char* sql = "SELECT * FROM games WHERE status = ? ORDER BY description;";
    
    sqlite3_stmt* stmt;
    std::vector<Game> games;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        games.push_back(buildGameFromQuery(stmt));
    }
    
    sqlite3_finalize(stmt);
    return games;
}

Game DatabaseManager::getGame(const std::string& game_name) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const char* sql = "SELECT * FROM games WHERE name = ?;";
    
    sqlite3_stmt* stmt;
    Game game;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return game;
    }
    
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        game = buildGameFromQuery(stmt);
        
        // Load ROMs for this game
        int64_t game_id = sqlite3_column_int64(stmt, 0); // Get game ID from main query
        const char* roms_sql = "SELECT name, size, crc, file_path FROM roms WHERE game_id = ?;";
        sqlite3_stmt* roms_stmt;
        
        if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(roms_stmt, 1, game_id);
            
            while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
                Rom rom;
                rom.name = safe_column_text(roms_stmt, 0);
                rom.size = sqlite3_column_int64(roms_stmt, 1);
                rom.crc = safe_column_text(roms_stmt, 2);
                game.roms.push_back(rom);
            }
            
            sqlite3_finalize(roms_stmt);
        }
    }
    
    sqlite3_finalize(stmt);
    return game;
}

Game DatabaseManager::getGame(const std::string& game_name, const std::string& system) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const char* sql = "SELECT * FROM games WHERE name = ? AND system = ?;";
    
    sqlite3_stmt* stmt;
    Game game;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return game;
    }
    
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, system.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        game = buildGameFromQuery(stmt);
        
        // Load ROMs for this game
        int64_t game_id = sqlite3_column_int64(stmt, 0); // Get game ID from main query
        const char* roms_sql = "SELECT name, size, crc, file_path FROM roms WHERE game_id = ?;";
        sqlite3_stmt* roms_stmt;
        
        if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(roms_stmt, 1, game_id);
            
            while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
                Rom rom;
                rom.name = safe_column_text(roms_stmt, 0);
                rom.size = sqlite3_column_int64(roms_stmt, 1);
                rom.crc = safe_column_text(roms_stmt, 2);
                game.roms.push_back(rom);
            }
            
            sqlite3_finalize(roms_stmt);
        }
    }
    
    sqlite3_finalize(stmt);
    return game;
}

std::vector<Game> DatabaseManager::getAllGamesWithName(const std::string& game_name) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<Game> games;
    const char* sql = "SELECT id, name, description, year, manufacturer, system, cloneof, romof, sourcefile, comment, video_type, orientation, width, height, aspect_x, aspect_y, driver_status, status, snapshot_path, dat_source FROM games WHERE name = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Game game;
        int64_t game_id = sqlite3_column_int64(stmt, 0);
        game.name = safe_column_text(stmt, 1);
        game.description = safe_column_text(stmt, 2);
        game.year = safe_column_text(stmt, 3);
        game.manufacturer = safe_column_text(stmt, 4);
        game.system = safe_column_text(stmt, 5);
        game.cloneof = safe_column_text(stmt, 6);
        game.romof = safe_column_text(stmt, 7);
        game.sourcefile = safe_column_text(stmt, 8);
        game.comment = safe_column_text(stmt, 9);
        game.video_type = safe_column_text(stmt, 10);
        game.orientation = safe_column_text(stmt, 11);
        game.width = safe_column_text(stmt, 12);
        game.height = safe_column_text(stmt, 13);
        game.aspect_x = safe_column_text(stmt, 14);
        game.aspect_y = safe_column_text(stmt, 15);
        game.driver_status = safe_column_text(stmt, 16);
        game.status = safe_column_text(stmt, 17);
        game.snapshot_path = safe_column_text(stmt, 18);
        game.dat_source = safe_column_text(stmt, 19);
        
        // Load ROMs for this game
        const char* roms_sql = "SELECT name, size, crc, file_path FROM roms WHERE game_id = ?;";
        sqlite3_stmt* roms_stmt;
        if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(roms_stmt, 1, game_id);
            while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
                Rom rom;
                rom.name = safe_column_text(roms_stmt, 0);
                rom.size = sqlite3_column_int64(roms_stmt, 1);
                rom.crc = safe_column_text(roms_stmt, 2);
                game.roms.push_back(rom);
            }
            
            sqlite3_finalize(roms_stmt);
        }
        
        games.push_back(game);
    }
    
    sqlite3_finalize(stmt);
    return games;
}

bool DatabaseManager::clearAllData() {
    const char* sql = "DELETE FROM roms; DELETE FROM games;";
    char* err_msg = nullptr;
    
    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression données: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

bool DatabaseManager::gameExists(const std::string& game_name) {
    const char* sql = "SELECT COUNT(*) FROM games WHERE name = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);

    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }

    sqlite3_finalize(stmt);
    return exists;
}

size_t DatabaseManager::getGameCount() {
    const char* sql = "SELECT COUNT(*) FROM games;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

bool DatabaseManager::beginTransaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, "BEGIN TRANSACTION;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur début transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::commitTransaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, "COMMIT;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur commit transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::rollbackTransaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, "ROLLBACK;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur rollback transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::createSavepoint(const std::string& name) {
    std::string sql = "SAVEPOINT " + name + ";";
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to create savepoint " << name << ": " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::releaseSavepoint(const std::string& name) {
    std::string sql = "RELEASE SAVEPOINT " + name + ";";
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to release savepoint " << name << ": " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::rollbackToSavepoint(const std::string& name) {
    std::string sql = "ROLLBACK TO SAVEPOINT " + name + ";";
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to rollback to savepoint " << name << ": " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

size_t DatabaseManager::getGameCountByStatus(const std::string& status) {
    const char* sql = "SELECT COUNT(*) FROM games WHERE status = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

// ── Favourites ───────────────────────────────────────────────────────────────

bool DatabaseManager::toggleFavorite(const std::string& game_name, const std::string& system) {
    const char* sql =
        "UPDATE games SET is_favorite = CASE WHEN is_favorite = 0 THEN 1 ELSE 0 END "
        "WHERE name = ? AND system = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, system.c_str(),    -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::isFavorite(const std::string& game_name, const std::string& system) {
    const char* sql = "SELECT is_favorite FROM games WHERE name = ? AND system = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, system.c_str(),    -1, SQLITE_STATIC);
    bool fav = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        fav = sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    return fav;
}

std::vector<Game> DatabaseManager::getFavorites() {
    const char* sql = "SELECT * FROM games WHERE is_favorite = 1 ORDER BY name;";
    sqlite3_stmt* stmt;
    std::vector<Game> result;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(buildGameFromQuery(stmt));
    sqlite3_finalize(stmt);
    return result;
}

// ── Play tracking ─────────────────────────────────────────────────────────────

bool DatabaseManager::recordLaunch(const std::string& game_name, const std::string& system) {
    // Get ISO-8601 timestamp
    time_t now = time(nullptr);
    char ts[32];
    struct tm* t = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);

    const char* sql =
        "UPDATE games SET last_played = ?, play_count = play_count + 1 "
        "WHERE name = ? AND system = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, ts,             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::addPlayTime(const std::string& game_name, const std::string& system, int seconds) {
    const char* sql =
        "UPDATE games SET play_time_secs = play_time_secs + ? WHERE name = ? AND system = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int (stmt, 1, seconds);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<Game> DatabaseManager::getRecentlyPlayed(int limit) {
    const char* sql =
        "SELECT * FROM games WHERE last_played IS NOT NULL "
        "ORDER BY last_played DESC LIMIT ?;";
    sqlite3_stmt* stmt;
    std::vector<Game> result;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(buildGameFromQuery(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool DatabaseManager::registerDatFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, int games_count) {
    const char* sql = "INSERT OR REPLACE INTO dat_files (filename, filepath, last_modified, file_size, games_count) VALUES (?, ?, ?, ?, ?);";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, filepath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, last_modified);
    sqlite3_bind_int64(stmt, 4, file_size);
    sqlite3_bind_int(stmt, 5, games_count);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::isDatFileUpToDate(const std::string& filename, time_t last_modified, size_t file_size) {
    const char* sql = "SELECT last_modified, file_size FROM dat_files WHERE filename = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    
    bool up_to_date = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t stored_modified = sqlite3_column_int64(stmt, 0);
        size_t stored_size = sqlite3_column_int64(stmt, 1);
        up_to_date = (stored_modified == last_modified && stored_size == file_size);
    }
    
    sqlite3_finalize(stmt);
    return up_to_date;
}

std::vector<std::string> DatabaseManager::getOutdatedDatFiles(const std::string& dat_directory) {
    std::vector<std::string> outdated_files;
    
    if (!std::filesystem::exists(dat_directory)) {
        return outdated_files;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(dat_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dat") {
            std::string filename = entry.path().filename().string();
            auto ftime = std::filesystem::last_write_time(entry);
            time_t last_modified = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            size_t file_size = std::filesystem::file_size(entry);
            
            if (!isDatFileUpToDate(filename, last_modified, file_size)) {
                outdated_files.push_back(filename);
            }
        }
    }
    
    return outdated_files;
}

bool DatabaseManager::removeGamesFromDat(const std::string& filename) {
    // Delete the child ROM rows BEFORE the parent games: foreign_keys is ON, so
    // deleting games first would violate roms.game_id -> games.id and fail.
    // (roms is keyed by game_id, not game_name — the old game_name predicate also
    // referenced a non-existent column.)
    const char* del_roms  = "DELETE FROM roms WHERE game_id IN (SELECT id FROM games WHERE dat_source = ?);";
    const char* del_games = "DELETE FROM games WHERE dat_source = ?;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, del_roms, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation removeGamesFromDat (roms): " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(m_db, del_games, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation removeGamesFromDat (games): " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        std::cout << "[INFO] Removed " << changes << " games from DAT: " << filename << std::endl;
        return true;
    }
    return false;
}

bool DatabaseManager::removeUnreferencedDatFiles(const std::vector<std::string>& existing_files) {
    // First, get list of DAT files that will be removed
    std::vector<std::string> removed_dat_files;
    
    if (existing_files.empty()) {
        // All DAT files will be removed - get all current ones
        const char* get_all_sql = "SELECT filename FROM dat_files;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, get_all_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                removed_dat_files.push_back(safe_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    } else {
        // Get DAT files not in existing_files list
        std::string query = "SELECT filename FROM dat_files WHERE filename NOT IN (";
        for (size_t i = 0; i < existing_files.size(); ++i) {
            if (i > 0) query += ",";
            query += "?";
        }
        query += ");";
        
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < existing_files.size(); ++i) {
                sqlite3_bind_text(stmt, i + 1, existing_files[i].c_str(), -1, SQLITE_STATIC);
            }
            
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                removed_dat_files.push_back(safe_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    }
    
    // Remove games from the DAT files that will be deleted
    for (const auto& dat_filename : removed_dat_files) {
        std::cout << "[SYNC] Suppression des jeux du DAT supprimé: " << dat_filename << std::endl;
        removeGamesFromDat(dat_filename);
    }
    
    // Remove DAT files from database that no longer exist on disk
    const char* sql = "DELETE FROM dat_files WHERE filename NOT IN (";
    
    if (existing_files.empty()) {
        const char* delete_all = "DELETE FROM dat_files;";
        return sqlite3_exec(m_db, delete_all, 0, 0, nullptr) == SQLITE_OK;
    }
    
    std::string query = sql;
    for (size_t i = 0; i < existing_files.size(); ++i) {
        if (i > 0) query += ",";
        query += "?";
    }
    query += ");";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    for (size_t i = 0; i < existing_files.size(); ++i) {
        sqlite3_bind_text(stmt, i + 1, existing_files[i].c_str(), -1, SQLITE_STATIC);
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

// ==================== INCREMENTAL DAT UPDATE ("DIFF") ====================

// Separator byte used to pack composite keys/values. 0x1F (US) never appears in
// game names, systems, ROM names or hex CRCs, so it is a safe delimiter.
static const char kSep = '\x1f';

std::unordered_map<std::string, std::string> DatabaseManager::snapshotStatusSignatures() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::unordered_map<std::string, std::string> out;

    // One pass over games joined with their ROMs, ordered so each game's ROM rows
    // are contiguous and deterministically ordered. The signature is the ordered
    // concatenation of "name:size:crc|" over the game's ROMs.
    const char* sql =
        "SELECT g.name, g.system, g.status, r.name, r.size, r.crc "
        "FROM games g LEFT JOIN roms r ON r.game_id = g.id "
        "ORDER BY g.name, g.system, r.name, r.size, r.crc;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] snapshotStatusSignatures prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return out;
    }

    std::string cur_key, cur_status, cur_sig;
    bool have_game = false;

    auto flush = [&]() {
        if (have_game) out[cur_key] = cur_status + kSep + cur_sig;
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string gname = safe_column_text(stmt, 0);
        std::string gsys  = safe_column_text(stmt, 1);
        std::string key = gname + kSep + gsys;

        if (!have_game || key != cur_key) {
            flush();
            cur_key = key;
            cur_status = safe_column_text(stmt, 2);
            cur_sig.clear();
            have_game = true;
        }

        // r.name is NULL for games with no ROMs (LEFT JOIN produced a single row).
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            cur_sig += safe_column_text(stmt, 3);
            cur_sig += ':';
            cur_sig += std::to_string(sqlite3_column_int64(stmt, 4));
            cur_sig += ':';
            cur_sig += safe_column_text(stmt, 5);
            cur_sig += '|';
        }
    }
    flush();

    sqlite3_finalize(stmt);
    return out;
}

int DatabaseManager::applyPreservedStatuses(
        const std::unordered_map<std::string, std::string>& old_snapshot,
        std::vector<std::string>& changed_zip_names) {
    // Signatures of the freshly reloaded database (statuses are all 'missing' here).
    std::unordered_map<std::string, std::string> current = snapshotStatusSignatures();

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    int restored = 0;
    bool own_tx = beginTransaction();

    for (const auto& [key, cur_val] : current) {
        size_t cur_sep = cur_val.find(kSep);
        std::string cur_sig = (cur_sep == std::string::npos) ? std::string() : cur_val.substr(cur_sep + 1);

        size_t key_sep = key.find(kSep);
        std::string gname = (key_sep == std::string::npos) ? key : key.substr(0, key_sep);
        std::string gsys  = (key_sep == std::string::npos) ? std::string() : key.substr(key_sep + 1);

        bool preserved = false;
        auto it = old_snapshot.find(key);
        if (it != old_snapshot.end()) {
            size_t old_sep = it->second.find(kSep);
            std::string old_status = (old_sep == std::string::npos) ? it->second : it->second.substr(0, old_sep);
            std::string old_sig    = (old_sep == std::string::npos) ? std::string() : it->second.substr(old_sep + 1);
            if (old_sig == cur_sig) {
                // Identical ROM definition => the previously computed status is still valid.
                if (old_status != "missing") {
                    updateGameStatus(gname, old_status, gsys);
                    ++restored;
                }
                preserved = true;
            }
        }

        if (!preserved) {
            // New game, or ROM definition changed: force its ZIP to be re-checked.
            changed_zip_names.push_back(gname + ".zip");
        }
    }

    if (own_tx) commitTransaction();
    return restored;
}

bool DatabaseManager::invalidateRomCacheForFiles(const std::vector<std::string>& zip_filenames) {
    if (zip_filenames.empty()) return true;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const char* sql = "DELETE FROM rom_cache WHERE filename = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] invalidateRomCacheForFiles prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    bool own_tx = beginTransaction();
    for (const auto& fn : zip_filenames) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, fn.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (own_tx) commitTransaction();
    return true;
}

bool DatabaseManager::storeZipContents(const std::string& filepath,
        const std::vector<std::pair<std::string, unsigned long>>& entries) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Store under the same canonical/sanitized path used by registerRomFile so the
    // two caches agree on the key.
    std::string store_path = filepath;
    try { store_path = std::filesystem::canonical(filepath).string(); } catch (...) {}
    store_path = sanitize_path(store_path);

    // Drop any previous contents for this file (they may have changed on disk).
    {
        sqlite3_stmt* del;
        if (sqlite3_prepare_v2(m_db, "DELETE FROM zip_contents WHERE filepath = ?;", -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del, 1, store_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
    }

    const char* sql = "INSERT OR REPLACE INTO zip_contents (filepath, entry_name, crc) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] storeZipContents prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    for (const auto& e : entries) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, store_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, e.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)e.second);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DatabaseManager::getAllZipContents(std::vector<ZipContentRow>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Ordered by filepath so callers can group a file's entries in a single pass.
    const char* sql = "SELECT filepath, entry_name, crc FROM zip_contents ORDER BY filepath;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ZipContentRow r;
        r.filepath   = safe_column_text(stmt, 0);
        r.entry_name = safe_column_text(stmt, 1);
        r.crc        = (unsigned long)sqlite3_column_int64(stmt, 2);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return true;
}

// ==================== ROM CACHE MANAGEMENT ====================

bool DatabaseManager::registerRomFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, time_t dat_timestamp) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Prepare once per call — use registerRomFilesBulk for high-volume insertion.
    const char* sql = "INSERT OR REPLACE INTO rom_cache "
                      "(filename, filepath, last_modified, file_size, last_scan_time, dat_timestamp, file_crc) "
                      "VALUES (?, ?, ?, ?, ?, ?, 0);";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] registerRomFile prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    // Store canonical path to avoid mismatch between runs (symlinks/relative paths).
    // The file was just scanned so it exists — skip the exists() check.
    std::string store_path = filepath;
    try { store_path = std::filesystem::canonical(filepath).string(); } catch (...) {}
    store_path = sanitize_path(store_path);

    time_t now = std::time(nullptr);
    sqlite3_bind_text(stmt, 1, filename.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, store_path.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, last_modified);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)file_size);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_int64(stmt, 6, dat_timestamp);
    // file_crc is 0: we rely on (size + mtime) for cache validity.
    // Whole-file CRC is only computed on demand in isRomFileCached() as a fallback.

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "[ERROR] registerRomFile insert failed for " << filename << ": " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    return true;
}

bool DatabaseManager::isRomFileCached(const std::string& filepath, time_t last_modified, size_t file_size, time_t current_dat_timestamp) {
    const char* sql = "SELECT last_modified, file_size, dat_timestamp, file_crc FROM rom_cache WHERE filepath = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    // Use canonical path for lookup to match stored values
    std::string lookup_path = filepath;
    try {
        if (std::filesystem::exists(filepath)) lookup_path = std::filesystem::canonical(filepath).string();
    } catch (...) { lookup_path = filepath; }
    sqlite3_bind_text(stmt, 1, lookup_path.c_str(), -1, SQLITE_TRANSIENT);

    bool is_cached = false;

    int step_result = sqlite3_step(stmt);
    if (step_result == SQLITE_ROW) {
        time_t cached_modified = sqlite3_column_int64(stmt, 0);
        size_t cached_size = sqlite3_column_int64(stmt, 1);
        time_t cached_dat_timestamp = sqlite3_column_int64(stmt, 2);
        long cached_file_crc = static_cast<long>(sqlite3_column_int64(stmt, 3));

        // If DAT timestamp changed, we must re-scan
            const long mtime_tolerance_seconds = 2; // tolerate small timestamp differences
            if (cached_dat_timestamp != current_dat_timestamp) {
                is_cached = false;
            } else if (cached_size == file_size && std::llabs(static_cast<long long>(cached_modified) - static_cast<long long>(last_modified)) <= mtime_tolerance_seconds) {
                // Size matches and mtime within tolerance => consider cached
                is_cached = true;
            } else {
            // Metadata differs (timestamps or size). As a fallback, compute file CRC and compare
            long current_crc = 0;
            try {
                current_crc = compute_file_crc32(lookup_path);
            } catch (...) {
                current_crc = 0;
            }

            if (current_crc != 0 && cached_file_crc != 0 && current_crc == cached_file_crc) {
                // File contents identical despite metadata differences - treat as cached
                is_cached = true;
            } else {
                is_cached = false;
            }
        }
    }

    sqlite3_finalize(stmt);
    return is_cached;
}

bool DatabaseManager::clearRomCache() {
    // Clear both the metadata cache and the content-addressed cache — they describe
    // the same physical files.
    const char* sql = "DELETE FROM rom_cache; DELETE FROM zip_contents;";
    char* err_msg = nullptr;

    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression cache ROM: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool DatabaseManager::clearDirectorySnapshots() {
    const char* sql = "DELETE FROM directory_snapshots;";
    char* err_msg = nullptr;

    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression directory_snapshots: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool DatabaseManager::clearDirectoryFiles() {
    const char* sql = "DELETE FROM directory_files;";
    char* err_msg = nullptr;

    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression directory_files: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::getDirectoryFileList(const std::string& path, std::vector<DirFileInfo>& files) {
    const char* sql = "SELECT filename, file_size, last_modified FROM directory_files "
                      "WHERE path = ? ORDER BY filename;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        DirFileInfo f;
        f.filename = safe_column_text(stmt, 0);
        f.size     = sqlite3_column_int64(stmt, 1);
        f.mtime    = sqlite3_column_int64(stmt, 2);
        files.push_back(std::move(f));
    }
    sqlite3_finalize(stmt);
    return found;
}

bool DatabaseManager::updateDirectoryFileList(const std::string& path, const std::vector<DirFileInfo>& files) {
    // Delete existing entries for this path and insert the new list
    const char* delete_sql = "DELETE FROM directory_files WHERE path = ?;";
    sqlite3_stmt* del_stmt;
    if (sqlite3_prepare_v2(m_db, delete_sql, -1, &del_stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(del_stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del_stmt);
    sqlite3_finalize(del_stmt);

    const char* insert_sql = "INSERT OR REPLACE INTO directory_files (path, filename, file_size, last_modified) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* ins_stmt;
    if (sqlite3_prepare_v2(m_db, insert_sql, -1, &ins_stmt, nullptr) != SQLITE_OK) return false;

    for (const auto& f : files) {
        sqlite3_reset(ins_stmt);
        sqlite3_bind_text(ins_stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins_stmt, 2, f.filename.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins_stmt, 3, f.size);
        sqlite3_bind_int64(ins_stmt, 4, f.mtime);
        sqlite3_step(ins_stmt);
    }
    sqlite3_finalize(ins_stmt);
    return true;
}

bool DatabaseManager::getSavedRomRoots(std::vector<std::string>& roots) {
    const char* sql = "SELECT path FROM saved_roots ORDER BY path;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        roots.push_back(safe_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DatabaseManager::updateSavedRomRoots(const std::vector<std::string>& roots) {
    // Replace current saved_roots with provided list
    const char* delete_sql = "DELETE FROM saved_roots;";
    char* err_msg = nullptr;
    if (sqlite3_exec(m_db, delete_sql, 0, 0, &err_msg) != SQLITE_OK) {
        sqlite3_free(err_msg);
        return false;
    }

    const char* insert_sql = "INSERT OR REPLACE INTO saved_roots (path) VALUES (?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    for (const auto& p : roots) {
        std::string store = p;
        try {
            if (!p.empty() && std::filesystem::exists(p)) {
                store = std::filesystem::canonical(p).string();
            }
        } catch (...) { store = p; }
        // normalize: remove trailing slash
        if (!store.empty() && store.back() == '/') store.pop_back();
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, store.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DatabaseManager::removeEntriesForRoot(const std::string& root_path) {
    // Compute canonical root with trailing slash
    std::string canon_root = root_path;
    try {
        if (!root_path.empty() && std::filesystem::exists(root_path)) {
            canon_root = std::filesystem::canonical(root_path).string();
        }
    } catch (...) { canon_root = root_path; }
    if (!canon_root.empty() && canon_root.back() != '/') canon_root.push_back('/');

    // Collect matching filepaths
    const char* select_sql = "SELECT filepath FROM rom_cache;";
    sqlite3_stmt* sel_stmt;
    if (sqlite3_prepare_v2(m_db, select_sql, -1, &sel_stmt, nullptr) != SQLITE_OK) return false;
    std::vector<std::string> to_delete;
    while (sqlite3_step(sel_stmt) == SQLITE_ROW) {
        std::string fp = safe_column_text(sel_stmt, 0);
        std::string fp_canon;
        try { fp_canon = std::filesystem::canonical(fp).string(); } catch (...) { fp_canon = fp; }
        std::string check = fp_canon;
        if (!check.empty() && check.back() != '/') check.push_back('/');
        if (check.rfind(canon_root, 0) == 0) {
            to_delete.push_back(fp);
        }
    }
    sqlite3_finalize(sel_stmt);

    // Safety checks: avoid accidental deletion of entire cache.
    if (canon_root.empty() || canon_root == "/") {
        std::cerr << "[WARN] removeEntriesForRoot: invalid canonical root '" << canon_root << "' - aborting" << std::endl;
        return false;
    }
        // Use SQL to delete rom_cache rows whose filepath starts with the canonical root + '/'
        int total_cache = 0;
        const char* count_sql = "SELECT COUNT(*) FROM rom_cache;";
        sqlite3_stmt* count_stmt;
        if (sqlite3_prepare_v2(m_db, count_sql, -1, &count_stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(count_stmt) == SQLITE_ROW) total_cache = sqlite3_column_int(count_stmt, 0);
            sqlite3_finalize(count_stmt);
        }

        // Prepare pattern like '/canonical/root/%'
        std::string pattern = canon_root;
        if (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
        pattern += "/%";

        int delete_count = 0;
        const char* count_del_sql = "SELECT COUNT(*) FROM rom_cache WHERE filepath LIKE ?;";
        sqlite3_stmt* count_del_stmt;
        if (sqlite3_prepare_v2(m_db, count_del_sql, -1, &count_del_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(count_del_stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(count_del_stmt) == SQLITE_ROW) delete_count = sqlite3_column_int(count_del_stmt, 0);
            sqlite3_finalize(count_del_stmt);
        }

        std::cerr << "[DBG removeEntriesForRoot] canon_root='" << canon_root << "' pattern='" << pattern << "' total_cache=" << total_cache << " delete_count=" << delete_count << std::endl;

        // Log sample filepaths that would be deleted (up to 100) to help debugging
        if (delete_count > 0) {
            const char* sample_sql = "SELECT filepath FROM rom_cache WHERE filepath LIKE ? LIMIT 100;";
            sqlite3_stmt* samp_stmt;
            if (sqlite3_prepare_v2(m_db, sample_sql, -1, &samp_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(samp_stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
                int idx = 0;
                while (sqlite3_step(samp_stmt) == SQLITE_ROW) {
                    std::string p = safe_column_text(samp_stmt, 0);
                    std::cerr << "[DBG removeEntriesForRoot] candidate[" << idx << "] = '" << p << "'" << std::endl;
                    idx++;
                }
                sqlite3_finalize(samp_stmt);
            }
        }

        // Safety: never allow deleting entire cache
        if (delete_count > 0 && total_cache > 0 && delete_count == total_cache) {
            std::cerr << "[WARN] removeEntriesForRoot: would remove all " << total_cache << " cache entries for root '" << canon_root << "' - aborting to avoid catastrophic removal" << std::endl;
            return false;
        }

        // Additional safety guard: refuse deletion that would remove a large portion
        const double DELETE_THRESHOLD = 0.50; // 50%
        if (delete_count > 0 && total_cache > 0 && delete_count >= static_cast<int>(std::ceil(total_cache * DELETE_THRESHOLD))) {
            std::cerr << "[WARN] removeEntriesForRoot: delete_count=" << delete_count << " exceeds threshold (" << (DELETE_THRESHOLD*100) << "%) of total_cache=" << total_cache << " - aborting" << std::endl;
            return false;
        }

        if (delete_count > 0) {
            const char* delete_sql = "DELETE FROM rom_cache WHERE filepath LIKE ?;";
            sqlite3_stmt* del_stmt;
            if (sqlite3_prepare_v2(m_db, delete_sql, -1, &del_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(del_stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(del_stmt);
                sqlite3_finalize(del_stmt);
                std::cout << "[CACHE CLEANUP] Removed " << delete_count << " rom_cache entries under root " << canon_root << std::endl;
            }
        }

    // Remove directory_snapshots and directory_files for paths under root
    const char* select_snap_sql = "SELECT path FROM directory_snapshots;";
    sqlite3_stmt* snap_stmt;
    if (sqlite3_prepare_v2(m_db, select_snap_sql, -1, &snap_stmt, nullptr) == SQLITE_OK) {
        std::vector<std::string> snap_delete;
        while (sqlite3_step(snap_stmt) == SQLITE_ROW) {
            std::string p = safe_column_text(snap_stmt, 0);
            std::string p_canon;
            try { p_canon = std::filesystem::canonical(p).string(); } catch (...) { p_canon = p; }
            std::string check = p_canon;
            if (!check.empty() && check.back() != '/') check.push_back('/');
            if (check.rfind(canon_root, 0) == 0) snap_delete.push_back(p);
        }
        sqlite3_finalize(snap_stmt);

        const char* del_snap_sql = "DELETE FROM directory_snapshots WHERE path = ?;";
        for (const auto& p : snap_delete) {
            sqlite3_stmt* ds;
            if (sqlite3_prepare_v2(m_db, del_snap_sql, -1, &ds, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(ds, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(ds);
                sqlite3_finalize(ds);
            }
        }
    }

    const char* select_df_sql = "SELECT DISTINCT path FROM directory_files;";
    sqlite3_stmt* df_stmt;
    if (sqlite3_prepare_v2(m_db, select_df_sql, -1, &df_stmt, nullptr) == SQLITE_OK) {
        std::vector<std::string> df_delete;
        while (sqlite3_step(df_stmt) == SQLITE_ROW) {
            std::string p = safe_column_text(df_stmt, 0);
            std::string p_canon;
            try { p_canon = std::filesystem::canonical(p).string(); } catch (...) { p_canon = p; }
            std::string check = p_canon;
            if (!check.empty() && check.back() != '/') check.push_back('/');
            if (check.rfind(canon_root, 0) == 0) df_delete.push_back(p);
        }
        sqlite3_finalize(df_stmt);

        const char* del_df_sql = "DELETE FROM directory_files WHERE path = ?;";
        for (const auto& p : df_delete) {
            sqlite3_stmt* dd;
            if (sqlite3_prepare_v2(m_db, del_df_sql, -1, &dd, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(dd, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(dd);
                sqlite3_finalize(dd);
            }
        }
    }

    return true;
}

// Unconditionally purge all rom_cache, directory_snapshot and directory_files
// entries that live under root_path. No safety threshold — this is called only
// when the user explicitly removes a configured ROM root from settings, so the
// deletion is intentional.
bool DatabaseManager::purgeCacheForRoot(const std::string& root_path) {
    std::string canon_root = root_path;
    try {
        if (!root_path.empty()) {
            // The directory may no longer exist on disk after removal — don't require it to exist.
            if (std::filesystem::exists(root_path))
                canon_root = std::filesystem::canonical(root_path).string();
        }
    } catch (...) { canon_root = root_path; }
    if (!canon_root.empty() && canon_root.back() == '/') canon_root.pop_back();
    if (canon_root.empty() || canon_root == "/") return false; // safety: never nuke everything

    std::string prefix = canon_root + "/";

    // Delete from rom_cache
    {
        const char* sql = "DELETE FROM rom_cache WHERE filepath LIKE ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string pattern = prefix + "%";
            sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(stmt);
            int deleted = sqlite3_changes(m_db);
            sqlite3_finalize(stmt);
            std::cerr << "[purgeCacheForRoot] rom_cache: deleted " << deleted << " entries under " << canon_root << std::endl;
        }
    }

    // Delete from directory_snapshots
    {
        const char* sql = "DELETE FROM directory_snapshots WHERE path = ? OR path LIKE ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string pattern = prefix + "%";
            sqlite3_bind_text(stmt, 1, canon_root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Delete from directory_files
    {
        const char* sql = "DELETE FROM directory_files WHERE path = ? OR path LIKE ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string pattern = prefix + "%";
            sqlite3_bind_text(stmt, 1, canon_root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return true;
}

bool DatabaseManager::reevaluateGamesAvailability(const std::vector<std::string>& roms_paths) {
    // Re-evaluate all games currently marked as available or incorrect
    std::vector<Game> avail = getGamesByStatus("available");
    std::vector<Game> incorrect = getGamesByStatus("incorrect");

    std::vector<Game> to_check;
    to_check.reserve(avail.size() + incorrect.size());
    to_check.insert(to_check.end(), avail.begin(), avail.end());
    to_check.insert(to_check.end(), incorrect.begin(), incorrect.end());

    for (auto& game : to_check) {
        RomScanner::check_availability(game, roms_paths);
        updateGameStatus(game.name, game.status, game.system);
    }

    return true;
}

int DatabaseManager::getRomCacheCount() {
    const char* sql = "SELECT COUNT(*) FROM rom_cache;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] getRomCacheCount prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

bool DatabaseManager::cleanupRomCache(const std::vector<std::string>& rom_paths) {
    // Get all cached file paths
    const char* sql = "SELECT filepath FROM rom_cache;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    std::vector<std::string> files_to_remove;

    // Precompute canonical rom_paths so we can check whether a cached filepath
    // is still under any of the configured ROM directories.
    std::vector<std::string> canon_rom_paths;
    for (const auto& p : rom_paths) {
        try {
            if (!p.empty() && std::filesystem::exists(p)) {
                std::string c = std::filesystem::canonical(p).string();
                if (!c.empty() && c.back() != '/') c.push_back('/');
                canon_rom_paths.push_back(c);
            } else {
                std::string c = p;
                if (!c.empty() && c.back() != '/') c.push_back('/');
                canon_rom_paths.push_back(c);
            }
        } catch (...) {
            std::string c = p;
            if (!c.empty() && c.back() != '/') c.push_back('/');
            canon_rom_paths.push_back(c);
        }
    }

    // If no configured rom paths (or none could be canonicalized), do not remove anything here.
    // This prevents accidental full-cache deletion when the application has no configured ROM roots.
    std::vector<std::string> filtered_roots;
    for (auto &rp : canon_rom_paths) {
        if (!rp.empty() && rp != "/") filtered_roots.push_back(rp);
    }
    canon_rom_paths.swap(filtered_roots);
    if (canon_rom_paths.empty()) {
        std::cerr << "[CACHE CLEANUP] No valid configured ROM roots provided — skipping cleanup to avoid mass deletion" << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* filepath_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!filepath_cstr) continue;

        std::string filepath = filepath_cstr;

        bool remove_entry = false;

        // If file no longer exists on disk -> remove
        bool exists_on_disk = false;
        try {
            exists_on_disk = std::filesystem::exists(filepath);
        } catch (...) { exists_on_disk = false; }
        if (!exists_on_disk) {
            remove_entry = true;
            files_to_remove.push_back(filepath);
            continue;
        }

        // Compute canonical filepath; if we cannot canonicalize, skip removing this entry.
        std::string fp_canon;
        try {
            fp_canon = std::filesystem::canonical(filepath).string();
        } catch (...) {
            // Skip this file - we can't reliably compare
            continue;
        }
        if (!fp_canon.empty() && fp_canon.back() != '/') fp_canon.push_back('/');

        // If canonical filepath is not under any configured canonical root, mark for removal
        bool under_any = false;
        for (const auto& rp : canon_rom_paths) {
            if (!rp.empty() && fp_canon.rfind(rp, 0) == 0) { under_any = true; break; }
        }
        if (!under_any) files_to_remove.push_back(filepath);
    }

    sqlite3_finalize(stmt);

    // Remove entries that are no longer valid
    if (!files_to_remove.empty()) {
        // Get current total cache count for safety checks
        int total_cache_now = getRomCacheCount();
        const double DELETE_THRESHOLD = 0.50; // 50%
        if (total_cache_now > 0 && static_cast<int>(files_to_remove.size()) >= static_cast<int>(std::ceil(total_cache_now * DELETE_THRESHOLD))) {
            std::cerr << "[WARN] cleanupRomCache: would remove " << files_to_remove.size() << " entries (>= " << (DELETE_THRESHOLD*100) << "% of current cache " << total_cache_now << ") - aborting to avoid catastrophic removal" << std::endl;
            return false;
        }

        std::cout << "[CACHE CLEANUP] Removing " << files_to_remove.size() << " files from cache (not under configured paths or deleted)" << std::endl;

        // Print sample of files to remove (first 100) to help debugging
        for (size_t i = 0; i < files_to_remove.size() && i < 100; ++i) {
            std::cerr << "[DBG cleanupRomCache] to_remove[" << i << "] = '" << files_to_remove[i] << "'" << std::endl;
        }

        const char* delete_sql = "DELETE FROM rom_cache WHERE filepath = ?;";
        for (const auto& filepath : files_to_remove) {
            sqlite3_stmt* delete_stmt;
            if (sqlite3_prepare_v2(m_db, delete_sql, -1, &delete_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(delete_stmt, 1, filepath.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(delete_stmt);
                sqlite3_finalize(delete_stmt);
            }
        }
    }

    return true;
}

void DatabaseManager::startCacheCleanupThread(const std::vector<std::string>& rom_paths) {
    if (m_cache_cleanup_running.load()) return;
    m_cache_cleanup_paths = rom_paths;
    m_cache_cleanup_running.store(true);
    m_cache_cleanup_thread = std::thread(cache_cleanup_thread_func, this, m_cache_cleanup_paths, &m_cache_cleanup_running);
}

void DatabaseManager::stopCacheCleanupThread() {
    if (!m_cache_cleanup_running.load()) return;
    m_cache_cleanup_running.store(false);
    if (m_cache_cleanup_thread.joinable()) m_cache_cleanup_thread.join();
}

time_t DatabaseManager::getLastDatTimestamp() {
    const char* sql = "SELECT value FROM scan_metadata WHERE key = 'last_dat_timestamp';";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    time_t timestamp = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        timestamp = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return timestamp;
}

bool DatabaseManager::setLastDatTimestamp(time_t timestamp) {
    const char* sql = "INSERT OR REPLACE INTO scan_metadata (key, value) VALUES ('last_dat_timestamp', ?);";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, timestamp);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<std::string> DatabaseManager::getOutdatedRomFiles(const std::vector<std::string>& rom_paths, time_t current_dat_timestamp, bool recursive, bool include_loose_files) {
    std::vector<std::string> outdated_files;

    for (const auto& rom_path : rom_paths) {
        if (!std::filesystem::exists(rom_path)) {
            continue;
        }

        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(rom_path)) {
                if (!entry.is_regular_file()) continue;

                std::string filepath = entry.path().string();

                // Skip DAT files (handled elsewhere)
                if (entry.path().extension() == ".dat") continue;

                // If not including loose files, only consider .zip
                if (!include_loose_files && entry.path().extension() != ".zip") continue;

                auto ftime = std::filesystem::last_write_time(entry);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                time_t last_modified = std::chrono::system_clock::to_time_t(sctp);
                size_t file_size = std::filesystem::file_size(entry);

                if (!isRomFileCached(filepath, last_modified, file_size, current_dat_timestamp)) {
                    outdated_files.push_back(filepath);
                }
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(rom_path)) {
                if (!entry.is_regular_file()) continue;

                std::string filepath = entry.path().string();
                if (entry.path().extension() == ".dat") continue;
                if (!include_loose_files && entry.path().extension() != ".zip") continue;

                auto ftime = std::filesystem::last_write_time(entry);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                time_t last_modified = std::chrono::system_clock::to_time_t(sctp);
                size_t file_size = std::filesystem::file_size(entry);

                if (!isRomFileCached(filepath, last_modified, file_size, current_dat_timestamp)) {
                    outdated_files.push_back(filepath);
                }
            }
        }
    }

    return outdated_files;
}

static std::string crc_to_hex8(unsigned long crc) {
    std::ostringstream ss;
    ss << std::hex << std::noshowbase << std::setw(8) << std::setfill('0') << (crc & 0xFFFFFFFFUL);
    std::string s = ss.str();
    // normalize to lowercase
    for (auto &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

std::vector<Game> DatabaseManager::getGamesByRomCrc(unsigned long crc) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<Game> results;
    const char* sql = "SELECT g.id, g.name, g.system FROM roms r JOIN games g ON r.game_id = g.id WHERE LOWER(r.crc) = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;

    std::string hex = crc_to_hex8(crc);
    sqlite3_bind_text(stmt, 1, hex.c_str(), -1, SQLITE_TRANSIENT);

    std::set<std::pair<std::string,std::string>> seen;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string name = safe_column_text(stmt, 1);
        std::string system = safe_column_text(stmt, 2);
        if (seen.insert({name, system}).second) {
            Game g = getGame(name, system);
            if (!g.name.empty()) results.push_back(g);
        }
    }
    sqlite3_finalize(stmt);
    return results;
}

bool DatabaseManager::getDirectorySnapshot(const std::string& path, int& file_count, time_t& last_modified) {
    const char* sql = "SELECT file_count, last_modified FROM directory_snapshots WHERE path = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        file_count = static_cast<int>(sqlite3_column_int(stmt, 0));
        last_modified = static_cast<time_t>(sqlite3_column_int64(stmt, 1));
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool DatabaseManager::updateDirectorySnapshot(const std::string& path, int file_count, time_t last_modified) {
    const char* sql = "INSERT OR REPLACE INTO directory_snapshots (path, file_count, last_modified) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, file_count);
    sqlite3_bind_int64(stmt, 3, static_cast<long long>(last_modified));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}