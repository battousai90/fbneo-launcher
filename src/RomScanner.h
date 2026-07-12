// src/RomScanner.h
#pragma once
#include "Game.h"
#include "DatabaseManager.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <utility>

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

    // Scan a ZIP and return results without touching DB (thread-pool path).
    // If out_entries is non-null it is filled with the ZIP's {entry_name, crc}
    // contents so the caller can persist them to the content-addressed cache.
    static std::vector<ScanResult> scan_zip_file_collect(const std::string& zip_path,
                                                          std::shared_ptr<DatabaseManager> db,
                                                          std::vector<std::pair<std::string, unsigned long>>* out_entries = nullptr);

    // Re-derive game availability from the content-addressed cache (zip_contents),
    // with NO disk I/O. Used after a DAT update to resolve new/changed games.
    // Returns the number of games upgraded to available/incorrect.
    static int rematch_from_cache(std::shared_ptr<DatabaseManager> db);

    // Database-specific availability check (used by scan_zip_file)
    static void check_availability_db(const std::string& game_name, const std::string& system,
                                       std::shared_ptr<DatabaseManager> db, const std::string& roms_path);
};