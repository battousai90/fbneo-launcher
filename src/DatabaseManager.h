// src/DatabaseManager.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <ctime>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <functional>
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
    
    // Incremental DAT update ("diff") support.
    // A snapshot maps "name\x1fsystem" -> "status\x1fromsignature", where the ROM
    // signature is a content fingerprint built from the game's ROM set (name/size/crc).
    // Two games with the same signature have an identical ROM definition, so a game
    // whose signature is unchanged across a DAT reload keeps its previously computed
    // availability status without needing to re-read its ROM files from disk.
    std::unordered_map<std::string, std::string> snapshotStatusSignatures();
    // After a DAT reload, restore statuses for every game whose (name, system) and
    // ROM signature are unchanged versus the given snapshot. Games that are new or
    // whose signature changed keep the default 'missing' status and have their ZIP
    // filename ("<name>.zip") appended to changed_zip_names so the caller can
    // invalidate exactly those cache entries. Returns the number of restored games.
    int applyPreservedStatuses(const std::unordered_map<std::string, std::string>& old_snapshot,
                               std::vector<std::string>& changed_zip_names);
    // Remove rom_cache rows whose filename matches any of the given ZIP filenames,
    // forcing those (and only those) ZIPs to be re-read on the next scan.
    bool invalidateRomCacheForFiles(const std::vector<std::string>& zip_filenames);

    // Content-addressed ROM cache (Phase 2): persist and read back the {entry, crc}
    // contents of scanned ZIPs so game availability can be re-derived without disk I/O.
    struct ZipContentRow { std::string filepath; std::string entry_name; unsigned long crc; };
    bool storeZipContents(const std::string& filepath,
                          const std::vector<std::pair<std::string, unsigned long>>& entries);
    bool getAllZipContents(std::vector<ZipContentRow>& out);

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
    // progress_cb, when set, is invoked periodically (not per file) with the
    // number of files checked so far and the root currently being walked — this
    // step does a filesystem stat + DB lookup per file with no other feedback,
    // so on a large or slow (e.g. external/network) drive it can otherwise look
    // hung for minutes with nothing on screen to prove it isn't. Returning false
    // aborts the walk (cooperative cancellation — checked at the same cadence
    // the callback fires, not per file).
    std::vector<std::string> getOutdatedRomFiles(const std::vector<std::string>& rom_paths, time_t current_dat_timestamp, bool recursive = true, bool include_loose_files = true,
                                                  std::function<bool(size_t, const std::string&)> progress_cb = nullptr);
    // Find candidate games that reference a ROM with the given CRC (crc as uLong)
    std::vector<Game> getGamesByRomCrc(unsigned long crc);
    // Directory snapshot helpers (used for light pre-scan to detect folder changes)
    bool getDirectorySnapshot(const std::string& path, int& file_count, time_t& last_modified);
    bool updateDirectorySnapshot(const std::string& path, int file_count, time_t last_modified);
    // Store/inspect directory file lists to detect adds/removes/renames — and
    // rewrites. Size + mtime matter: a rebuilt ROM set (RomVault, torrentzip)
    // keeps every filename identical, so a name-only comparison would report
    // "no change" and leave the old, wrong statuses in place.
    struct DirFileInfo {
        std::string filename;
        long long   size  = 0;
        long long   mtime = 0;
        bool operator==(const DirFileInfo& o) const {
            return filename == o.filename && size == o.size && mtime == o.mtime;
        }
        bool operator!=(const DirFileInfo& o) const { return !(*this == o); }
        bool operator<(const DirFileInfo& o) const { return filename < o.filename; }
    };
    bool getDirectoryFileList(const std::string& path, std::vector<DirFileInfo>& files);
    bool updateDirectoryFileList(const std::string& path, const std::vector<DirFileInfo>& files);
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
    // Guards methods called from the parallel scan workers (read-only) so that
    // they don't interleave with concurrent transactions on the same handle.
    // Recursive because transactional callers may invoke other helpers that
    // also lock (e.g. updateGameStatusWithSource from within a BEGIN...COMMIT).
    mutable std::recursive_mutex m_mutex;
    // Background cache cleanup
    std::thread m_cache_cleanup_thread;
    std::atomic<bool> m_cache_cleanup_running{false};
    std::vector<std::string> m_cache_cleanup_paths;
    
    bool createTables();
    Game buildGameFromQuery(sqlite3_stmt* stmt);
    // Same check as isRomFileCached(), but against a statement the caller already
    // prepared — getOutdatedRomFiles() walks potentially tens of thousands of
    // files, and re-preparing this query per file was the single biggest cost
    // in that loop.
    bool isRomFileCachedStmt(sqlite3_stmt* stmt, const std::string& filepath, time_t last_modified, size_t file_size, time_t current_dat_timestamp);
};