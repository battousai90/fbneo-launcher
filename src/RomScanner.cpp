
// src/RomScanner.cpp
#include "RomScanner.h"
#include <filesystem>
#include <zlib.h>
#include <zip.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstdint>
#include <map>
#include <unordered_map>

static bool find_rom_by_crc_in_zip(const std::string& zip_path, uLong expected_crc);

// Normalize filename for comparison (handle : vs - differences)
static std::string normalize_filename(const std::string& filename) {
    std::string normalized = filename;
    // Replace - with : in title areas (before year parentheses)
    size_t first_paren = normalized.find('(');
    if (first_paren != std::string::npos) {
        for (size_t i = 0; i < first_paren; ++i) {
            if (normalized[i] == '-' && i > 0 && normalized[i-1] != ' ' && i < normalized.length()-1 && normalized[i+1] == ' ') {
                normalized[i] = ':';
            }
        }
    }
    return normalized;
}

static bool find_rom_by_crc_in_zip(const std::string& zip_path, uLong expected_crc) {
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return false;

    zip_int64_t num_entries = zip_get_num_entries(zip, 0);
    for (zip_uint64_t i = 0; i < num_entries; ++i) {
        zip_file_t* zip_file = zip_fopen_index(zip, i, 0);
        if (!zip_file) continue;

        uLong crc = crc32(0L, Z_NULL, 0);
        char buffer[8192];
        int len;
        while ((len = zip_fread(zip_file, buffer, sizeof(buffer))) > 0) {
            crc = crc32(crc, (const Bytef*)buffer, len);
        }
        zip_fclose(zip_file);

        if (crc == expected_crc) {
            zip_close(zip);
            return true;
        }
    }
    zip_close(zip);
    return false;
}

static uLong compute_crc32(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        crc = crc32(crc, (const Bytef*)buffer, file.gcount());
    }
    return crc;
}

uLong compute_crc32_in_zip(const std::string& zip_path, const std::string& rom_name) {
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return 0;
    
    // Try exact name first
    zip_file_t* zip_file = zip_fopen(zip, rom_name.c_str(), 0);
    
    // If not found, try searching with normalization
    if (!zip_file) {
        zip_int64_t num_entries = zip_get_num_entries(zip, 0);
        std::string normalized_target = normalize_filename(rom_name);
        
        for (zip_uint64_t i = 0; i < num_entries; ++i) {
            zip_stat_t sb;
            if (zip_stat_index(zip, i, 0, &sb) == 0) {
                std::string entry_name = sb.name;
                std::string normalized_entry = normalize_filename(entry_name);
                
                if (normalized_entry == normalized_target) {
                    zip_file = zip_fopen(zip, entry_name.c_str(), 0);
                    break;
                }
            }
        }
    }
    
    if (!zip_file) {
        zip_close(zip);
        return 0;
    }
    
    uLong crc = crc32(0L, Z_NULL, 0);
    char buffer[8192];
    int len;
    while ((len = zip_fread(zip_file, buffer, sizeof(buffer))) > 0) {
        crc = crc32(crc, (const Bytef*)buffer, len);
    }
    zip_fclose(zip_file);
    zip_close(zip);
    return crc;
}

uLong hex_to_crc(const std::string& hex) {
    uLong crc;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> crc;
    return crc;
}

// Shared matching core: given a game and a ZIP's {name→crc} / {crc→name} maps,
// decide the game's status. Used by both the live scan and the cache re-match so
// the two can never diverge.
static std::string check_game_maps(const Game& game,
                                   const std::unordered_map<std::string, uLong>& crc_by_name,
                                   const std::unordered_map<uLong, std::string>& name_by_crc) {
    if (game.roms.empty()) return "";
    bool all_present = true, all_correct = true;
    for (const auto& rom : game.roms) {
        if (rom.crc.empty()) continue; // optional ROM, skip
        uLong expected = hex_to_crc(rom.crc);
        auto it = crc_by_name.find(rom.name);
        if (it == crc_by_name.end())
            it = crc_by_name.find(normalize_filename(rom.name));
        if (it != crc_by_name.end()) {
            if (it->second != expected) all_correct = false;
        } else {
            if (name_by_crc.find(expected) == name_by_crc.end()) {
                all_present = false;
                break;
            }
            all_correct = false; // found by CRC but filename differs
        }
    }
    if (!all_present) return "missing";
    return all_correct ? "available" : "incorrect";
}

void RomScanner::check_availability(Game& game, const std::string& roms_path) {
    // If no ROMs defined for this game, consider it missing (can't verify)
    if (game.roms.empty()) {
        game.status = "missing";
        return;
    }

    bool all_present = true;
    bool all_correct = true;
    

    for (const auto& rom : game.roms) {
        std::string rom_path = roms_path + "/" + rom.name;
        std::string zip_path = roms_path + "/" + game.name + ".zip";
        bool rom_found = false;

        // Check for individual ROM file
        if (std::filesystem::exists(rom_path)) {
            rom_found = true;
            size_t file_size = std::filesystem::file_size(rom_path);
            if (file_size != rom.size) {
                all_correct = false;
                continue;
            }

            uLong actual_crc = compute_crc32(rom_path);
            uLong expected_crc = hex_to_crc(rom.crc);
            if (actual_crc != expected_crc) {
                all_correct = false;
            }
        }
        // Check for ROM in ZIP file - first try by filename, then by CRC
        else if (std::filesystem::exists(zip_path)) {
            uLong actual_crc = compute_crc32_in_zip(zip_path, rom.name);
            uLong expected_crc = hex_to_crc(rom.crc);
            
            if (actual_crc != 0 && actual_crc == expected_crc) {
                // Found by filename and CRC matches
                rom_found = true;
            } else if (actual_crc == 0) {
                // Not found by filename, try searching by CRC
                if (find_rom_by_crc_in_zip(zip_path, expected_crc)) {
                    rom_found = true;
                } else {
                    rom_found = false;
                }
            } else {
                // Found by filename but wrong CRC
                rom_found = true;
                all_correct = false;
            }
        }

        // If this ROM is not found anywhere, game is missing
        if (!rom_found) {
            all_present = false;
            break;
        }
    }

    if (!all_present) {
        game.status = "missing";
    } else if (!all_correct) {
        game.status = "incorrect";
    } else {
        game.status = "available";
    }
}

void RomScanner::check_availability(Game& game, const std::vector<std::string>& roms_paths) {
    // Try each ROMs directory until we find a match or exhaust all paths
    for (const auto& roms_path : roms_paths) {
        check_availability(game, roms_path);
        
        
        // If we found the game (available or incorrect), stop searching
        if (game.status == "available" || game.status == "incorrect") {
            return;
        }
    }
    
    // If we get here, the game wasn't found in any directory
    game.status = "missing";
}

// DELETED - USELESS METHOD

// DELETED - SECOND USELESS METHOD

void RomScanner::check_availability_db(const std::string& game_name, const std::string& system, std::shared_ptr<DatabaseManager> db, const std::string& roms_path) {
    // Get the specific game by name AND system
    Game game = db->getGame(game_name, system);
    
    if (game.name.empty()) {
        return; // Game not found in database
    }

    // If no ROMs defined for this game, consider it missing
    if (game.roms.empty()) {
        db->updateGameStatus(game_name, "missing", system);
        return;
    }

    bool all_present = true;
    bool all_correct = true;
    
    std::string zip_path = roms_path + "/" + game_name + ".zip";
    
    // Debug for our 3 problematic games
    bool is_debug = (game_name == "sboy3" || game_name == "wonsiin" || game_name == "cyborgs");
    
    if (is_debug) {
        std::cout << "[DEBUG] FINAL CHECK: " << game_name << " [" << system << "] in " << roms_path << std::endl;
        std::cout << "[DEBUG] Looking for ZIP: " << zip_path << " (exists: " << std::filesystem::exists(zip_path) << ")" << std::endl;
    }
    
    for (const auto& rom : game.roms) {
        std::string rom_path = roms_path + "/" + rom.name;
        bool rom_found = false;

        // Check for individual ROM file first
        if (std::filesystem::exists(rom_path)) {
            rom_found = true;
            size_t file_size = std::filesystem::file_size(rom_path);
            if (file_size != rom.size) {
                all_correct = false;
                continue;
            }

            uLong actual_crc = compute_crc32(rom_path);
            uLong expected_crc = hex_to_crc(rom.crc);
            if (actual_crc != expected_crc) {
                all_correct = false;
            }
        }
        // Check for ROM in ZIP file - EXACT FILENAME + CRC ONLY
        else if (std::filesystem::exists(zip_path)) {
            uLong actual_crc = compute_crc32_in_zip(zip_path, rom.name);
            uLong expected_crc = hex_to_crc(rom.crc);
            
            if (is_debug) {
                std::cout << "[DEBUG]     ROM: " << rom.name << " - expected CRC: " << std::hex << expected_crc 
                         << ", actual CRC: " << actual_crc << std::dec << std::endl;
            }
            
            if (actual_crc != 0 && actual_crc == expected_crc) {
                // Found by EXACT filename AND CRC matches - PERFECT
                rom_found = true;
                if (is_debug) std::cout << "[DEBUG]       EXACT MATCH: filename AND CRC!" << std::endl;
            } else if (actual_crc == 0) {
                // ROM file not found by exact name in ZIP
                rom_found = false;
                if (is_debug) std::cout << "[DEBUG]       ROM file not found by exact name" << std::endl;
            } else {
                // Found by filename but wrong CRC
                rom_found = true;
                all_correct = false;
                if (is_debug) std::cout << "[DEBUG]       Found by name but wrong CRC!" << std::endl;
            }
        } else {
            // ZIP not found
            rom_found = false;
            if (is_debug) std::cout << "[DEBUG]   ZIP file not found" << std::endl;
        }

        // If this ROM is not found, game is missing
        if (!rom_found) {
            all_present = false;
            break;
        }
    }

    std::string status;
    if (!all_present) {
        status = "missing";
    } else if (!all_correct) {
        status = "incorrect";
    } else {
        status = "available";
    }
    
    if (is_debug) {
        std::cout << "[DEBUG] FINAL RESULT: " << game_name << " [" << system << "]: " << status << std::endl;
    }
    
    // Update status with system-specific lookup
    db->updateGameStatus(game_name, status, system);
}

// FAST SCAN METHOD — single-pass ZIP read.
// The ZIP is opened ONCE. All entries are read and their CRCs computed into an
// in-memory map, so every subsequent candidate check is a cheap map lookup
// instead of a repeated open/read/close cycle.
void RomScanner::scan_zip_file(const std::string& zip_path, std::shared_ptr<DatabaseManager> db) {
    std::string game_name = std::filesystem::path(zip_path).stem().string();

    // ── Phase 1: open ZIP once, build {filename → crc} map ──────────────────
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return;

    // Map: exact name → crc  (and normalised name → crc for fallback)
    std::unordered_map<std::string, uLong> crc_by_name;
    // Set of all CRCs present in the ZIP (for CRC-only discovery)
    std::unordered_map<uLong, std::string> name_by_crc;

    constexpr size_t BUF = 65536;
    std::vector<char> buf(BUF);

    zip_int64_t num_entries = zip_get_num_entries(zip, 0);
    for (zip_uint64_t i = 0; i < (zip_uint64_t)num_entries; ++i) {
        zip_stat_t sb;
        if (zip_stat_index(zip, i, 0, &sb) != 0) continue;
        std::string entry_name = sb.name;
        if (entry_name.empty() || entry_name.back() == '/') continue; // dir

        zip_file_t* zf = zip_fopen_index(zip, i, 0);
        if (!zf) continue;

        uLong crc = crc32(0L, Z_NULL, 0);
        zip_int64_t len;
        while ((len = zip_fread(zf, buf.data(), BUF)) > 0)
            crc = crc32(crc, (const Bytef*)buf.data(), (uInt)len);
        zip_fclose(zf);

        crc_by_name[entry_name] = crc;
        crc_by_name[normalize_filename(entry_name)] = crc;
        name_by_crc[crc] = entry_name; // first entry wins if dupe CRC
    }
    zip_close(zip);

    // ── Helper: check if ALL non-empty ROMs of a game are satisfied ──────────
    auto check_game = [&](const Game& game) -> std::string /* status */ {
        if (game.roms.empty()) return "";
        bool all_present = true;
        bool all_correct = true;

        for (const auto& rom : game.roms) {
            if (rom.crc.empty()) continue; // optional ROM, skip

            uLong expected = hex_to_crc(rom.crc);

            // Try exact filename match
            auto it = crc_by_name.find(rom.name);
            if (it == crc_by_name.end())
                it = crc_by_name.find(normalize_filename(rom.name));

            if (it != crc_by_name.end()) {
                if (it->second != expected) all_correct = false;
                // presence confirmed
            } else {
                // Not found by name; try CRC-only match
                if (name_by_crc.find(expected) == name_by_crc.end()) {
                    all_present = false;
                    break;
                }
                // Found by CRC but filename differs → incorrect
                all_correct = false;
            }
        }

        if (!all_present) return "missing";
        return all_correct ? "available" : "incorrect";
    };

    // ── Phase 2: name-based candidates ──────────────────────────────────────
    std::vector<Game> candidates = db->getAllGamesWithName(game_name);
    for (const auto& candidate : candidates) {
        Game game = db->getGame(candidate.name, candidate.system);
        std::string status = check_game(game);
        if (status == "available" || status == "incorrect")
            db->updateGameStatus(game.name, status, game.system);
    }

    // ── Phase 3: CRC-only discovery for ZIPs with no name match ─────────────
    if (candidates.empty()) {
        for (const auto& [crc, _] : name_by_crc) {
            std::vector<Game> crc_cands = db->getGamesByRomCrc(crc);
            for (const auto& cand : crc_cands) {
                Game game = db->getGame(cand.name, cand.system);
                std::string status = check_game(game);
                if (status == "available" || status == "incorrect")
                    db->updateGameStatus(game.name, status, game.system);
            }
        }
    }
}

// Thread-safe variant: same logic but returns results instead of writing to DB.
// Safe to call from multiple threads concurrently (only DB reads, no writes).
std::vector<RomScanner::ScanResult>
RomScanner::scan_zip_file_collect(const std::string& zip_path,
                                   std::shared_ptr<DatabaseManager> db,
                                   std::vector<std::pair<std::string, unsigned long>>* out_entries)
{
    std::vector<ScanResult> results;
    std::string game_name = std::filesystem::path(zip_path).stem().string();

    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return results;

    std::unordered_map<std::string, uLong> crc_by_name;
    std::unordered_map<uLong, std::string> name_by_crc;

    constexpr size_t BUF = 65536;
    std::vector<char> buf(BUF);
    zip_int64_t num_entries = zip_get_num_entries(zip, 0);
    for (zip_uint64_t i = 0; i < (zip_uint64_t)num_entries; ++i) {
        zip_stat_t sb;
        if (zip_stat_index(zip, i, 0, &sb) != 0) continue;
        std::string entry_name = sb.name;
        if (entry_name.empty() || entry_name.back() == '/') continue;

        zip_file_t* zf = zip_fopen_index(zip, i, 0);
        if (!zf) continue;
        uLong crc = crc32(0L, Z_NULL, 0);
        zip_int64_t len;
        while ((len = zip_fread(zf, buf.data(), BUF)) > 0)
            crc = crc32(crc, (const Bytef*)buf.data(), (uInt)len);
        zip_fclose(zf);

        crc_by_name[entry_name] = crc;
        crc_by_name[normalize_filename(entry_name)] = crc;
        name_by_crc[crc] = entry_name;

        // Capture the real (un-normalized) contents for the content-addressed cache.
        if (out_entries)
            out_entries->emplace_back(entry_name, (unsigned long)crc);
    }
    zip_close(zip);

    // Parent directory of the ZIP — stored as source_directory for each found game
    std::string source_dir = std::filesystem::path(zip_path).parent_path().string();

    // Name-based candidates (read-only DB queries — thread-safe)
    std::vector<Game> candidates = db->getAllGamesWithName(game_name);
    for (const auto& candidate : candidates) {
        Game game = db->getGame(candidate.name, candidate.system);
        std::string status = check_game_maps(game, crc_by_name, name_by_crc);
        if (status == "available" || status == "incorrect")
            results.push_back({game.name, game.system, status, source_dir});
    }

    // CRC-only discovery
    if (candidates.empty()) {
        for (const auto& [crc, _] : name_by_crc) {
            std::vector<Game> crc_cands = db->getGamesByRomCrc(crc);
            for (const auto& cand : crc_cands) {
                Game game = db->getGame(cand.name, cand.system);
                std::string status = check_game_maps(game, crc_by_name, name_by_crc);
                if (status == "available" || status == "incorrect")
                    results.push_back({game.name, game.system, status, source_dir});
            }
        }
    }

    return results;
}

// Re-derive availability from the content-addressed cache (zip_contents) — no disk I/O.
// Mirrors the live scan (name candidates + CRC-only discovery) but sources ZIP
// contents from the DB instead of reading files. Only upgrades statuses
// (missing → available/incorrect), never downgrades, exactly like a real scan.
int RomScanner::rematch_from_cache(std::shared_ptr<DatabaseManager> db) {
    std::vector<DatabaseManager::ZipContentRow> rows;
    if (!db->getAllZipContents(rows) || rows.empty()) return 0;

    int upgraded = 0;
    db->beginTransaction();

    size_t i = 0;
    while (i < rows.size()) {
        const std::string filepath = rows[i].filepath;

        std::unordered_map<std::string, uLong> crc_by_name;
        std::unordered_map<uLong, std::string> name_by_crc;
        while (i < rows.size() && rows[i].filepath == filepath) {
            const std::string& en = rows[i].entry_name;
            uLong crc = (uLong)rows[i].crc;
            crc_by_name[en] = crc;
            crc_by_name[normalize_filename(en)] = crc;
            name_by_crc[crc] = en;
            ++i;
        }

        // Skip stale cache entries for files that no longer exist on disk.
        std::error_code ec;
        if (!std::filesystem::exists(filepath, ec)) continue;

        std::string game_name = std::filesystem::path(filepath).stem().string();

        std::vector<Game> candidates = db->getAllGamesWithName(game_name);
        for (const auto& cand : candidates) {
            Game game = db->getGame(cand.name, cand.system);
            std::string status = check_game_maps(game, crc_by_name, name_by_crc);
            if (status == "available" || status == "incorrect") {
                db->updateGameStatus(game.name, status, game.system);
                ++upgraded;
            }
        }

        if (candidates.empty()) {
            for (const auto& [crc, _] : name_by_crc) {
                std::vector<Game> crc_cands = db->getGamesByRomCrc(crc);
                for (const auto& cand : crc_cands) {
                    Game game = db->getGame(cand.name, cand.system);
                    std::string status = check_game_maps(game, crc_by_name, name_by_crc);
                    if (status == "available" || status == "incorrect") {
                        db->updateGameStatus(game.name, status, game.system);
                        ++upgraded;
                    }
                }
            }
        }
    }

    db->commitTransaction();
    return upgraded;
}