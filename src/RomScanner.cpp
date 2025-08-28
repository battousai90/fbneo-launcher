
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

// FAST SCAN METHOD - Check candidates by ZIP name first
void RomScanner::scan_zip_file(const std::string& zip_path, std::shared_ptr<DatabaseManager> db) {
    std::string game_name = std::filesystem::path(zip_path).stem().string();
    
    // Get candidate games with this name
    std::vector<Game> candidates = db->getAllGamesWithName(game_name);
    if (candidates.empty()) return;
    
    // Open ZIP file
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return;
    
    // For each candidate game, check if ALL its ROMs are in ZIP with correct CRC
    for (const auto& candidate : candidates) {
        Game game = db->getGame(candidate.name, candidate.system);
        if (game.roms.empty()) continue;
        
        bool all_roms_present = true;
        
        for (const auto& rom : game.roms) {
            // Skip ROMs with empty CRC (optional files)
            if (rom.crc.empty()) {
                continue;
            }
            
            uLong actual_crc = compute_crc32_in_zip(zip_path, rom.name);
            uLong expected_crc = hex_to_crc(rom.crc);
            
            if (actual_crc == 0 || actual_crc != expected_crc) {
                all_roms_present = false;
                break;
            }
        }
        
        if (all_roms_present) {
            db->updateGameStatus(game.name, "available", game.system);
        }
    }
    
    zip_close(zip);
}