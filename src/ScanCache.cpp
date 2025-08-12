// src/ScanCache.cpp
#include "ScanCache.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>

bool ScanCache::save(const std::vector<Game>& games, const std::string& filename) {
    nlohmann::json j;
    for (const auto& game : games) {
        j.push_back({
            {"name", game.name},
            {"status", game.status},
            {"description", game.description},
            {"year", game.year},
            {"manufacturer", game.manufacturer}
        });
    }

    std::ofstream file(filename);
    if (!file.is_open()) return false;
    file << j.dump(4);
    return true;
}

bool ScanCache::load(std::vector<Game>& games, const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Cannot open cache file: " << filename << std::endl;
        return false;
    }

    try {
        nlohmann::json j;
        file >> j;

        if (!j.is_array()) {
            std::cerr << "[ERROR] Invalid cache format in: " << filename << std::endl;
            return false;
        }
        games.clear();

        for (const auto& jgame : j) {
            Game game;
            game.name = jgame.at("name");
            game.status = jgame.at("status");
            game.description = jgame.value("description", "Unknown");
            game.year = jgame.value("year", "0000");
            game.manufacturer = jgame.value("manufacturer", "Unknown");
            games.push_back(game);
        }
        return true;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[ERROR] JSON parsing error: " << e.what() << std::endl;
        return false;
    }
}