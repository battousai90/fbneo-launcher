// src/DatabaseManager.cpp
#include "DatabaseManager.h"
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <chrono>

// Helper function to safely get text from SQLite column
static std::string safe_column_text(sqlite3_stmt* stmt, int column) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    return text ? std::string(text) : std::string("");
}

DatabaseManager::DatabaseManager(const std::string& db_path) : m_db(nullptr), m_db_path(db_path) {}

DatabaseManager::~DatabaseManager() {
    if (m_db) {
        sqlite3_close(m_db);
    }
}

bool DatabaseManager::initialize() {
    int rc = sqlite3_open(m_db_path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur ouverture base de données: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    return createTables();
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
    
    const char* create_index_sql = R"(
        CREATE INDEX IF NOT EXISTS idx_games_name ON games(name);
        CREATE INDEX IF NOT EXISTS idx_games_system ON games(system);
        CREATE INDEX IF NOT EXISTS idx_games_status ON games(status);
        CREATE INDEX IF NOT EXISTS idx_roms_game_id ON roms(game_id);
        CREATE INDEX IF NOT EXISTS idx_dat_files_filename ON dat_files(filename);
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
    
    if (sqlite3_exec(m_db, create_index_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création index: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
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
    const char* sql = "SELECT id, name, description, year, manufacturer, system, cloneof, romof, sourcefile, comment, video_type, orientation, width, height, aspect_x, aspect_y, driver_status, status, snapshot_path, dat_source FROM games ORDER BY description;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    
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

Game DatabaseManager::buildGameFromQuery(sqlite3_stmt* stmt) {
    Game game;
    game.name = safe_column_text(stmt, 1);
    game.description = safe_column_text(stmt, 2);
    game.year = safe_column_text(stmt, 3);
    game.manufacturer = safe_column_text(stmt, 4);
    game.system = safe_column_text(stmt, 5);
    game.status = safe_column_text(stmt, 6);
    game.video_type = safe_column_text(stmt, 7);
    game.orientation = safe_column_text(stmt, 8);
    game.width = safe_column_text(stmt, 9);
    game.height = safe_column_text(stmt, 10);
    game.aspect_x = safe_column_text(stmt, 11);
    game.aspect_y = safe_column_text(stmt, 12);
    game.driver_status = safe_column_text(stmt, 13);
    game.comment = safe_column_text(stmt, 14);
    game.cloneof = safe_column_text(stmt, 15);
    game.romof = safe_column_text(stmt, 16);
    game.sourcefile = safe_column_text(stmt, 17);
    game.snapshot_path = safe_column_text(stmt, 18);
    game.dat_source = safe_column_text(stmt, 19);
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

bool DatabaseManager::registerDatFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, int games_count) {
    const char* sql = "INSERT OR REPLACE INTO dat_files (filename, filepath, last_modified, file_size, games_count) VALUES (?, ?, ?, ?, ?);";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, filepath.c_str(), -1, SQLITE_STATIC);
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
    // Remove all games that come from this DAT file
    const char* sql = "DELETE FROM games WHERE dat_source = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation removeGamesFromDat: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        std::cout << "[INFO] Supprimé " << changes << " jeux du DAT: " << filename << std::endl;
        
        // Also remove ROMs that no longer have associated games
        const char* cleanup_roms = "DELETE FROM roms WHERE game_name NOT IN (SELECT name FROM games);";
        sqlite3_exec(m_db, cleanup_roms, 0, 0, nullptr);
        
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