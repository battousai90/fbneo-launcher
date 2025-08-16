#include "src/RomScanner.h"
#include "src/Game.h"
#include <iostream>

int main() {
    Game game;
    game.name = "007tld";
    
    Rom rom;
    rom.name = "007 - The Living Daylights (Euro, GB)(1987)(Domark)[RUN'CAS-'].cas";
    rom.size = 47046;
    rom.crc = "6eeb8d76";
    game.roms.push_back(rom);
    
    std::string roms_path = "/mnt/roms/FinalBurnNeo/FinalBurn Neo - MSX 1 Games";
    
    std::cout << "Testing game: " << game.name << std::endl;
    std::cout << "ROMs path: " << roms_path << std::endl;
    
    RomScanner::check_availability(game, roms_path);
    
    std::cout << "Final status: " << game.status << std::endl;
    
    return 0;
}