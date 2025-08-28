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
    
    // NEW CLEAN SCAN METHOD - THE ONLY ONE NEEDED
    static void scan_zip_file(const std::string& zip_path, std::shared_ptr<DatabaseManager> db);
    
    // Database-specific availability check (used by scan_zip_file)
    static void check_availability_db(const std::string& game_name, const std::string& system, std::shared_ptr<DatabaseManager> db, const std::string& roms_path);
};