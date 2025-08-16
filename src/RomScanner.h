// src/RomScanner.h
#pragma once
#include "Game.h"
#include <string>
#include <vector>

class RomScanner {
public:
    static void check_availability(Game& game, const std::string& roms_path);
    static void check_availability(Game& game, const std::vector<std::string>& roms_paths);
};