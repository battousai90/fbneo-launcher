// src/ScanCache.h
#pragma once
#include <vector>
#include <string>
#include "Game.h"

class ScanCache {
public:
    static bool save(const std::vector<Game>& games, const std::string& filename);
    static bool load(std::vector<Game>& games, const std::string& filename);
};