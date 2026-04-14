// src/DatabaseManager.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <ctime>
#include <thread>
#include <atomic>
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
    bool updateGameStatusWithSource(const std::string& game_name, const std::string& status, const std::string& system, const std::string& source_directory);
    bool resetGamesFromDirectory(const std::string& directory);
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

    // Favourites
    bool toggleFavorite(const std::string& game_name, const std::string& system);
    bool isFavorite(const std::string& game_name, const std::string& system);
    std::vector<Game> getFavorites();

    // Play tracking
    bool recordLaunch(const std::string& game_name, const std::string& system);
    bool addPlayTime(const std::string& game_name, const std::string& system, int seconds);
    std::vector<Game> getRecentlyPlayed(int limit = 20);

    bool clearAllData();
    bool gameExists(const std::string& game_name);

    // Transaction management
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // Savepoint management (for nested transactions)
    bool createSavepoint(const std::string& name);
    bool releaseSavepoint(const std::string& name);
    bool rollbackToSavepoint(const std::string& name);
    
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
    bool clearDirectorySnapshots();
    bool cleanupRomCache(const std::vector<std::string>& rom_paths);
    // Background cache cleanup thread
    void startCacheCleanupThread(const std::vector<std::string>& rom_paths);
    void stopCacheCleanupThread();
    int getRomCacheCount();
    time_t getLastDatTimestamp();
    bool setLastDatTimestamp(time_t timestamp);
    std::vector<std::string> getOutdatedRomFiles(const std::vector<std::string>& rom_paths, time_t current_dat_timestamp, bool recursive = true, bool include_loose_files = true);
    // Find candidate games that reference a ROM with the given CRC (crc as uLong)
    std::vector<Game> getGamesByRomCrc(unsigned long crc);
    // Directory snapshot helpers (used for light pre-scan to detect folder changes)
    bool getDirectorySnapshot(const std::string& path, int& file_count, time_t& last_modified);
    bool updateDirectorySnapshot(const std::string& path, int file_count, time_t last_modified);
    // Store/inspect directory file lists to detect adds/removes/renames
    bool getDirectoryFileList(const std::string& path, std::vector<std::string>& filenames);
    bool updateDirectoryFileList(const std::string& path, const std::vector<std::string>& filenames);
    bool clearDirectoryFiles();
    // Persist configured ROM roots so we can detect removed roots between scans
    bool getSavedRomRoots(std::vector<std::string>& roots);
    bool updateSavedRomRoots(const std::vector<std::string>& roots);
    // Remove all cache/snapshot entries under a given root path
    bool removeEntriesForRoot(const std::string& root_path);
    // Unconditional purge of cache entries for an explicitly-removed root (no safety threshold)
    bool purgeCacheForRoot(const std::string& root_path);
    // Re-evaluate availability statuses for games after cache changes
    bool reevaluateGamesAvailability(const std::vector<std::string>& roms_paths);
    
private:
    sqlite3* m_db;
    std::string m_db_path;
    // Background cache cleanup
    std::thread m_cache_cleanup_thread;
    std::atomic<bool> m_cache_cleanup_running{false};
    std::vector<std::string> m_cache_cleanup_paths;
    
    bool createTables();
    Game buildGameFromQuery(sqlite3_stmt* stmt);
};