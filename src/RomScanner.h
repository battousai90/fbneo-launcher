// src/RomScanner.h
#pragma once
#include "Game.h"
#include <string>

class RomScanner {
public:
    static void check_availability(Game& game, const std::string& roms_path);
};