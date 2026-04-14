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
    struct ScanResult {
        std::string name;
        std::string system;
        std::string status;           // "available", "incorrect", "missing" — empty = no change
        std::string source_directory; // parent directory of the scanned ZIP
    };

    static void check_availability(Game& game, const std::string& roms_path);
    static void check_availability(Game& game, const std::vector<std::string>& roms_paths);

    // Scan a ZIP and write results directly to DB (single-threaded path)
    static void scan_zip_file(const std::string& zip_path, std::shared_ptr<DatabaseManager> db);

    // Scan a ZIP and return results without touching DB (thread-pool path)
    static std::vector<ScanResult> scan_zip_file_collect(const std::string& zip_path,
                                                          std::shared_ptr<DatabaseManager> db);

    // Database-specific availability check (used by scan_zip_file)
    static void check_availability_db(const std::string& game_name, const std::string& system,
                                       std::shared_ptr<DatabaseManager> db, const std::string& roms_path);
};