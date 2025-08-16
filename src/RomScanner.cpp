// src/RomScanner.cpp
#include "RomScanner.h"
#include <filesystem>
#include <zlib.h>       // For CRC32
#include <zip.h>
#include <fstream>
#include <iostream>
#include <sstream>


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


static uLong compute_crc32_in_zip(const std::string& zip_path, const std::string& rom_name) {
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

static uLong hex_to_crc(const std::string& hex) {
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
        // Check for ROM in ZIP file
        else if (std::filesystem::exists(zip_path)) {
            uLong actual_crc = compute_crc32_in_zip(zip_path, rom.name);
            uLong expected_crc = hex_to_crc(rom.crc);
            if (actual_crc != 0 && actual_crc == expected_crc) {
                rom_found = true;
            } else {
                all_correct = false;
                if (actual_crc == 0) {
                    rom_found = false;
                } else {
                    rom_found = true; // Found but incorrect
                }
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