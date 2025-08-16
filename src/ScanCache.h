// src/ScanCache.h
#pragma once
#include <vector>
#include <string>
#include "Game.h"

class ScanCache {
public:
    static bool save(const std::vector<Game>& games, const std::string& filename);
    static bool save(const std::vector<Game>& games, const std::vector<std::string>& roms_paths, const std::string& filename);
    static bool load(std::vector<Game>& games, const std::string& filename);
    static bool load(std::vector<Game>& games, std::vector<std::string>& roms_paths, const std::string& filename);
    static bool is_cache_valid(const std::vector<std::string>& current_roms_paths, const std::string& filename);
};