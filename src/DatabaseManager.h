// src/DatabaseManager.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <ctime>
#include "Game.h"

struct sqlite3;
struct sqlite3_stmt;

class DatabaseManager {
public:
    DatabaseManager(const std::string& db_path);
    ~DatabaseManager();
    
    bool initialize();
    bool insertGame(const Game& game);
    bool insertRom(int64_t game_id, const Rom& rom);
    bool updateRomPath(const std::string& game_name, const std::string& rom_name, const std::string& file_path);
    bool updateGameStatus(const std::string& game_name, const std::string& status);
    bool updateGameStatus(const std::string& game_name, const std::string& status, const std::string& system);
    bool updateGameSnapshot(const std::string& game_name, const std::string& snapshot_path);
    bool resetAllGamesToMissing();
    
    std::vector<Game> getAllGames();
    std::vector<Game> getGamesBySystem(const std::string& system);
    std::vector<Game> getGamesByStatus(const std::string& status);
    Game getGame(const std::string& game_name);
    Game getGame(const std::string& game_name, const std::string& system);
    std::vector<Game> getAllGamesWithName(const std::string& game_name);
    size_t getGameCount();
    size_t getGameCountByStatus(const std::string& status);

    bool clearAllData();
    bool gameExists(const std::string& game_name);

    // Transaction management
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
    
    // DAT file management
    bool registerDatFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, int games_count);
    bool isDatFileUpToDate(const std::string& filename, time_t last_modified, size_t file_size);
    std::vector<std::string> getOutdatedDatFiles(const std::string& dat_directory);
    bool removeGamesFromDat(const std::string& filename);
    bool removeUnreferencedDatFiles(const std::vector<std::string>& existing_files);

    // ROM cache management
    bool registerRomFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, time_t dat_timestamp);
    bool isRomFileCached(const std::string& filepath, time_t last_modified, size_t file_size, time_t current_dat_timestamp);
    bool clearRomCache();
    bool cleanupRomCache(const std::vector<std::string>& rom_paths);
    time_t getLastDatTimestamp();
    bool setLastDatTimestamp(time_t timestamp);
    std::vector<std::string> getOutdatedRomFiles(const std::vector<std::string>& rom_paths, time_t current_dat_timestamp);
    
private:
    sqlite3* m_db;
    std::string m_db_path;
    
    bool createTables();
    Game buildGameFromQuery(sqlite3_stmt* stmt);
};