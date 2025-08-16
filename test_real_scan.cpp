#include "src/RomScanner.h"
#include "src/DatParser.h"
#include "src/Game.h"
#include <iostream>

int main() {
    // Load games exactly like the launcher does
    std::string dat_path = "/home/gilbert/DEV/fbneo-launcher/data";
    auto games = DatParser::parseAllDats(dat_path);
    
    std::cout << "Loaded " << games.size() << " games from DAT files" << std::endl;
    
    // Test just the first MSX game we know exists
    Game* test_game = nullptr;
    for (auto& game : games) {
        if (game.name == "007tld") {
            test_game = &game;
            break;
        }
    }
    
    if (!test_game) {
        std::cout << "ERROR: 007tld game not found in DAT files!" << std::endl;
        return 1;
    }
    
    std::cout << "Found game: " << test_game->name << " with " << test_game->roms.size() << " ROMs" << std::endl;
    for (const auto& rom : test_game->roms) {
        std::cout << "  ROM: " << rom.name << " size: " << rom.size << " crc: " << rom.crc << std::endl;
    }
    
    // Test with the exact ROM paths from config
    std::vector<std::string> roms_paths = {"/mnt/roms/FinalBurnNeo/FinalBurn Neo - MSX 1 Games"};
    
    std::cout << "\\nTesting scan with paths:" << std::endl;
    for (const auto& path : roms_paths) {
        std::cout << "  " << path << std::endl;
    }
    
    RomScanner::check_availability(*test_game, roms_paths);
    
    std::cout << "\\nResult: " << test_game->status << std::endl;
    
    // Now count how many games would be found in a full scan
    int found_count = 0;
    int total_checked = 0;
    for (auto& game : games) {
        if (total_checked < 100) { // Check first 100 games
            RomScanner::check_availability(game, roms_paths);
            if (game.status == "available" || game.status == "incorrect") {
                found_count++;
                std::cout << "Found: " << game.name << " (" << game.status << ")" << std::endl;
            }
            total_checked++;
        }
    }
    
    std::cout << "\\nOut of " << total_checked << " games checked, " << found_count << " ROMs were found" << std::endl;
    
    return 0;
}