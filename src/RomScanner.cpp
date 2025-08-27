
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

static bool find_rom_by_crc_in_zip(const std::string& zip_path, uLong expected_crc);

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
    zip_file_t* zip_file = zip_fopen(zip, rom_name.c_str(), 0);
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

void RomScanner::check_availability_db(const std::string& game_name, std::shared_ptr<DatabaseManager> db, const std::string& roms_path) {
    // Detect system from ROM directory path
    std::string system = "";
    std::string dir_name = std::filesystem::path(roms_path).filename().string();
    if (dir_name.find("SG-1000") != std::string::npos) {
        system = "Sega SG-1000";
    } else if (dir_name.find("Neo Geo Pocket") != std::string::npos) {
        system = "Neo Geo Pocket";
    } else if (dir_name.find("Fairchild") != std::string::npos || dir_name.find("Channel F") != std::string::npos) {
        system = "Fairchild Channel F";
    } else if (dir_name.find("TurboGrafx") != std::string::npos || dir_name.find("PC Engine") != std::string::npos) {
        system = "TurboGrafx-16";
    } else if (dir_name.find("Game Gear") != std::string::npos) {
        system = "Game Gear";
    }
    
    // Get game from database with system-specific lookup
    Game game;
    if (!system.empty()) {
        game = db->getGame(game_name, system);
    } else {
        game = db->getGame(game_name);
    }
    
    if (game.name.empty()) {
        return; // Game not found in database
    }

    // If no ROMs defined for this game, consider it missing
    if (game.roms.empty()) {
        db->updateGameStatus(game_name, "missing");
        return;
    }

    bool all_present = true;
    bool all_correct = true;
    
    // Debug for first few games
    static int debug_count = 0;
    bool is_debug = (debug_count < 3);
    if (is_debug) {
        std::cout << "[DEBUG] Checking game: " << game_name << " with " << game.roms.size() << " ROMs" << std::endl;
        debug_count++;
    }
    
    std::string zip_path = roms_path + "/" + game_name + ".zip";
    if (is_debug) {
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
        // Check for ROM in ZIP file - first by filename, then by CRC
        else if (std::filesystem::exists(zip_path)) {
            uLong actual_crc = compute_crc32_in_zip(zip_path, rom.name);
            uLong expected_crc = hex_to_crc(rom.crc);
            
            if (is_debug) {
                std::cout << "[DEBUG]   ROM: " << rom.name << " - expected CRC: " << std::hex << expected_crc 
                         << ", actual CRC: " << actual_crc << std::dec << std::endl;
            }
            
            if (actual_crc != 0 && actual_crc == expected_crc) {
                // Found by filename and CRC matches
                rom_found = true;
                if (is_debug) std::cout << "[DEBUG]     CRC match by filename!" << std::endl;
            } else if (actual_crc == 0) {
                // Not found by filename, try searching by CRC
                if (find_rom_by_crc_in_zip(zip_path, expected_crc)) {
                    rom_found = true;
                    if (is_debug) std::cout << "[DEBUG]     CRC match found by search!" << std::endl;
                } else {
                    rom_found = false;
                    if (is_debug) std::cout << "[DEBUG]     ROM not found in ZIP" << std::endl;
                }
            } else {
                // Found by filename but wrong CRC
                rom_found = true;
                all_correct = false;
                if (is_debug) std::cout << "[DEBUG]     CRC mismatch!" << std::endl;
            }
        } else {
            // Neither individual file nor ZIP exists
            rom_found = false;
            if (is_debug) std::cout << "[DEBUG]   ZIP file not found" << std::endl;
        }

        // If this ROM is not found anywhere, game is missing
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
        std::cout << "[DEBUG] Final status for " << game_name << " [" << system << "]: " << status << std::endl;
    }
    
    // Update status with system-specific lookup
    if (!system.empty()) {
        db->updateGameStatus(game_name, status, system);
    } else {
        db->updateGameStatus(game_name, status);
    }
}

void RomScanner::check_availability_db(const std::string& game_name, std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths) {
    // Try each ROMs directory until we find a match or exhaust all paths
    for (const auto& roms_path : roms_paths) {
        check_availability_db(game_name, db, roms_path);
        
        // Get current status from database
        Game game = db->getGame(game_name);
        if (game.status == "available" || game.status == "incorrect") {
            return; // Found the game, stop searching
        }
    }
    
    // If we get here, the game wasn't found in any directory
    db->updateGameStatus(game_name, "missing");
}

void RomScanner::check_availability_db(const std::string& game_name, const std::string& system, std::shared_ptr<DatabaseManager> db, const std::string& roms_path) {
    // Get the specific game by name AND system
    Game game = db->getGame(game_name, system);
    
    if (game.name.empty()) {
        return; // Game not found in database
    }

    // If no ROMs defined for this game, consider it missing
    if (game.roms.empty()) {
        if (!system.empty()) {
            db->updateGameStatus(game_name, "missing", system);
        } else {
            db->updateGameStatus(game_name, "missing");
        }
        return;
    }

    bool all_present = true;
    bool all_correct = true;
    
    std::string zip_path = roms_path + "/" + game_name + ".zip";
    
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
        // Check for ROM in ZIP file - first by filename, then by CRC
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
        } else {
            // Neither individual file nor ZIP exists
            rom_found = false;
        }

        // If this ROM is not found anywhere, game is missing
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
    
    // Update status with system-specific lookup
    if (!system.empty()) {
        db->updateGameStatus(game_name, status, system);
    } else {
        db->updateGameStatus(game_name, status);
    }
}

void RomScanner::scan_all_games_db(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths) {
    scan_all_games_db(db, roms_paths, nullptr);
}

void RomScanner::scan_all_games_db(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths, std::function<void(int, int)> progress_callback) {
    std::vector<Game> games = db->getAllGames();
    
    std::cout << "Starting scan of " << games.size() << " games..." << std::endl;
    
    int scanned = 0;
    for (const auto& game : games) {
        check_availability_db(game.name, db, roms_paths);
        scanned++;
        
        if (progress_callback) {
            progress_callback(scanned, games.size());
        }
        
        if (scanned % 100 == 0) {
            std::cout << "Scanned " << scanned << "/" << games.size() << " games..." << std::endl;
        }
    }
    
    std::cout << "Scan completed!" << std::endl;
}

void RomScanner::scan_rom_directories_db(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths, std::function<void(int, int, const std::string&)> progress_callback) {
    std::cout << "Starting ROM directory scan..." << std::endl;
    
    // Get all games from database first
    std::vector<Game> all_games = db->getAllGames();
    std::cout << "Database contains " << all_games.size() << " games" << std::endl;
    
    // Reset all games to missing status initially
    for (const auto& game : all_games) {
        db->updateGameStatus(game.name, "missing");
    }
    
    // Collect all ROM files from all directories
    std::vector<std::string> all_rom_files;
    for (const auto& roms_path : roms_paths) {
        if (!std::filesystem::exists(roms_path)) {
            std::cout << "[WARNING] ROM directory does not exist: " << roms_path << std::endl;
            continue;
        }
        
        std::cout << "Scanning ROM directory: " << roms_path << std::endl;
        for (const auto& entry : std::filesystem::directory_iterator(roms_path)) {
            if (entry.is_regular_file()) {
                std::string filepath = entry.path().string();
                std::string extension = entry.path().extension().string();
                
                // Check for supported ROM file types (.zip primarily for arcade)
                if (extension == ".zip") {
                    all_rom_files.push_back(filepath);
                }
            }
        }
    }
    
    std::cout << "Found " << all_rom_files.size() << " ROM files to process" << std::endl;
    
    int processed = 0;
    int found_games = 0;
    
    // Process each ROM file
    for (const auto& rom_file : all_rom_files) {
        if (progress_callback) {
            std::string filename = std::filesystem::path(rom_file).filename().string();
            progress_callback(processed, all_rom_files.size(), filename);
        }
        
        // Extract game name from filename (remove .zip extension)
        std::string game_name = std::filesystem::path(rom_file).stem().string();
        
        // Determine system from ROM directory path  
        std::string system = "";
        std::string dir_name = std::filesystem::path(rom_file).parent_path().filename().string();
        if (dir_name.find("SG-1000") != std::string::npos) {
            system = "Sega SG-1000";
        } else if (dir_name.find("Neo Geo Pocket") != std::string::npos) {
            system = "Neo Geo Pocket";
        } else if (dir_name.find("Arcade") != std::string::npos) {
            system = "Arcade";
        } else if (dir_name.find("Neogeo") != std::string::npos) {
            system = "Neo-Geo";
        }
        // Add more system mappings as needed
        
        // Try to find matching game in database for this specific system
        Game game;
        if (!system.empty()) {
            game = db->getGame(game_name, system);
        } else {
            game = db->getGame(game_name);
        }
        
        if (!game.name.empty()) {
            // Game exists in database, check its ROMs
            std::string roms_path = std::filesystem::path(rom_file).parent_path().string();
            check_availability_db(game_name, db, roms_path);
            
            // Get updated status
            Game updated_game = db->getGame(game_name);
            if (updated_game.status == "available" || updated_game.status == "incorrect") {
                found_games++;
                if (found_games <= 10) {
                    std::cout << "[DEBUG] Found game: " << game_name << " (" << updated_game.status << ")" << std::endl;
                }
            }
        }
        
        processed++;
        
        if (processed % 100 == 0) {
            std::cout << "Processed " << processed << "/" << all_rom_files.size() << " ROM files, found " << found_games << " games" << std::endl;
        }
    }
    
    std::cout << "ROM directory scan completed! Processed " << all_rom_files.size() << " files, found " << found_games << " games" << std::endl;
}