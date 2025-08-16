// src/ScanCache.cpp
#include "ScanCache.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>

bool ScanCache::save(const std::vector<Game>& games, const std::string& filename) {
    // Use the new save method with empty roms_paths for backward compatibility
    return save(games, std::vector<std::string>(), filename);
}

bool ScanCache::save(const std::vector<Game>& games, const std::vector<std::string>& roms_paths, const std::string& filename) {
    nlohmann::json j;
    
    // Add ROM paths to cache
    j["roms_paths"] = roms_paths;
    
    // Add games array
    j["games"] = nlohmann::json::array();
    for (const auto& game : games) {
        j["games"].push_back({
            {"name", game.name},
            {"status", game.status},
            {"description", game.description},
            {"year", game.year},
            {"manufacturer", game.manufacturer},
            {"system", game.system},
            {"video_type", game.video_type},
            {"orientation", game.orientation},
            {"width", game.width},
            {"height", game.height},
            {"aspect_x", game.aspect_x},
            {"aspect_y", game.aspect_y},
            {"driver_status", game.driver_status},
            {"comment", game.comment},
            {"cloneof", game.cloneof},
            {"romof", game.romof},
            {"sourcefile", game.sourcefile}
        });
    }

    std::ofstream file(filename);
    if (!file.is_open()) return false;
    file << j.dump(4);
    return true;
}

bool ScanCache::load(std::vector<Game>& games, const std::string& filename) {
    std::vector<std::string> dummy_paths;
    return load(games, dummy_paths, filename);
}

bool ScanCache::load(std::vector<Game>& games, std::vector<std::string>& roms_paths, const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Cannot open cache file: " << filename << std::endl;
        return false;
    }

    try {
        nlohmann::json j;
        file >> j;

        games.clear();
        roms_paths.clear();

        // Handle both old and new cache formats
        if (j.is_array()) {
            // Old format - just games array
            for (const auto& jgame : j) {
                Game game;
                game.name = jgame.at("name");
                game.status = jgame.at("status");
                game.description = jgame.value("description", "Unknown");
                game.year = jgame.value("year", "0000");
                game.manufacturer = jgame.value("manufacturer", "Unknown");
                game.system = jgame.value("system", "Unknown");
                game.video_type = jgame.value("video_type", "");
                game.orientation = jgame.value("orientation", "");
                game.width = jgame.value("width", "");
                game.height = jgame.value("height", "");
                game.aspect_x = jgame.value("aspect_x", "");
                game.aspect_y = jgame.value("aspect_y", "");
                game.driver_status = jgame.value("driver_status", "");
                game.comment = jgame.value("comment", "");
                game.cloneof = jgame.value("cloneof", "");
                game.romof = jgame.value("romof", "");
                game.sourcefile = jgame.value("sourcefile", "");
                games.push_back(game);
            }
        } else if (j.is_object()) {
            // New format - object with roms_paths and games
            if (j.contains("roms_paths")) {
                roms_paths = j["roms_paths"].get<std::vector<std::string>>();
            }
            
            if (j.contains("games") && j["games"].is_array()) {
                for (const auto& jgame : j["games"]) {
                    Game game;
                    game.name = jgame.at("name");
                    game.status = jgame.at("status");
                    game.description = jgame.value("description", "Unknown");
                    game.year = jgame.value("year", "0000");
                    game.manufacturer = jgame.value("manufacturer", "Unknown");
                    game.system = jgame.value("system", "Unknown");
                    game.video_type = jgame.value("video_type", "");
                    game.orientation = jgame.value("orientation", "");
                    game.width = jgame.value("width", "");
                    game.height = jgame.value("height", "");
                    game.aspect_x = jgame.value("aspect_x", "");
                    game.aspect_y = jgame.value("aspect_y", "");
                    game.driver_status = jgame.value("driver_status", "");
                    game.comment = jgame.value("comment", "");
                    game.cloneof = jgame.value("cloneof", "");
                    game.romof = jgame.value("romof", "");
                    game.sourcefile = jgame.value("sourcefile", "");
                    games.push_back(game);
                }
            }
        } else {
            std::cerr << "[ERROR] Invalid cache format in: " << filename << std::endl;
            return false;
        }
        
        return true;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[ERROR] JSON parsing error: " << e.what() << std::endl;
        return false;
    }
}

bool ScanCache::is_cache_valid(const std::vector<std::string>& current_roms_paths, const std::string& filename) {
    std::vector<Game> dummy_games;
    std::vector<std::string> cached_roms_paths;
    
    if (!load(dummy_games, cached_roms_paths, filename)) {
        std::cout << "[DEBUG] Cache load failed, cache invalid" << std::endl;
        return false;
    }
    
    std::cout << "[DEBUG] Current ROM paths (" << current_roms_paths.size() << "):" << std::endl;
    for (const auto& path : current_roms_paths) {
        std::cout << "[DEBUG]   - " << path << std::endl;
    }
    
    std::cout << "[DEBUG] Cached ROM paths (" << cached_roms_paths.size() << "):" << std::endl;
    for (const auto& path : cached_roms_paths) {
        std::cout << "[DEBUG]   - " << path << std::endl;
    }
    
    // Compare current ROM paths with cached ones
    bool valid = current_roms_paths == cached_roms_paths;
    std::cout << "[DEBUG] Cache validation result: " << (valid ? "VALID" : "INVALID") << std::endl;
    return valid;
}