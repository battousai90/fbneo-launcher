// src/RomScanner.h
#pragma once
#include "Game.h"
#include "DatabaseManager.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

class RomScanner {
public:
    static void check_availability(Game& game, const std::string& roms_path);
    static void check_availability(Game& game, const std::vector<std::string>& roms_paths);
    
    // Database-based scanning methods
    static void check_availability_db(const std::string& game_name, std::shared_ptr<DatabaseManager> db, const std::string& roms_path);
    static void check_availability_db(const std::string& game_name, std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths);
    static void check_availability_db(const std::string& game_name, const std::string& system, std::shared_ptr<DatabaseManager> db, const std::string& roms_path);
    static void scan_all_games_db(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths);
    static void scan_all_games_db(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths, std::function<void(int, int)> progress_callback);
    
    // New method that scans ROM directories and finds matching games in database
    static void scan_rom_directories_db(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths, std::function<void(int, int, const std::string&)> progress_callback);
};