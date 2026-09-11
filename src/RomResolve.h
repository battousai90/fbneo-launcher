// src/RomResolve.h
//
// The one place that decides whether a set is available, incorrect or missing.
//
// Three code paths used to carry their own copy of that rule : the live scan
// (one zip at a time, in parallel), the cache re-match after a DAT update, and
// the library audit. They agreed only as long as the rule stayed "every ROM the
// DAT lists must be in the set's own zip", which is the *non-merged* layout and
// nothing else. FBNeo DATs mark 85 % of Neo Geo ROMs and 56 % of arcade ROMs
// with merge= : in a *split* collection those live in the parent's zip (or the
// BIOS's), and a set that lacks them is complete, not broken.
//
// So the rule now takes the collection's style and, for inherited ROMs, a way
// to look at other archives. Callers plug in where those archives come from
// (a freshly read zip, the zip_contents cache, the audit's index); the rule
// itself never touches the disk or the database.
#pragma once

#include "DatabaseManager.h"
#include "Game.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RomResolve {

// How the collection is laid out. Merged (a clone has no zip of its own; its
// ROMs sit in the parent's) is a later phase and deliberately not listed yet :
// an enum value with no behaviour behind it would be a promise the code cannot
// keep.
enum class SetStyle { NonMerged, Split };

SetStyle    style_from_string(const std::string& s);   // unknown → NonMerged
std::string to_string(SetStyle s);
// config.json → rom_manager.set_style. Missing or unknown → NonMerged, which is
// exactly the behaviour every scan had before the setting existed.
SetStyle    load_style();

// What one archive holds, keyed the way the scanner keys it: every entry name
// under its raw spelling *and* under RomScanner::normalize_name(), so ':' vs
// '-' never makes a set look wrong to one consumer and fine to another.
struct Archive {
    std::string path;                                            // empty for an archive not on disk
    std::unordered_map<std::string, unsigned long> crc_by_name;
    std::unordered_map<unsigned long, std::string> name_by_crc;  // first entry wins
    std::vector<std::string> entries;                            // raw names, one per real entry
    void add(const std::string& entry_name, unsigned long crc);
};

// Where inherited ROMs are looked for. Returns null when that set has no
// archive in the collection. The chain is followed through Game::romof, so the
// second callback has to resolve a set name within a system.
using ArchiveLookup = std::function<const Archive*(const Game& game)>;
using GameLookup    = std::function<Game(const std::string& name, const std::string& system)>;

enum class RomState {
    Present,    // right data, under the right name
    WrongName,  // right data, stored under another name
    Corrupt,    // the expected name is there, but the data is wrong
    Absent,     // nowhere it is allowed to be
};

struct RomVerdict {
    std::string   name;
    unsigned long crc  = 0;
    uint64_t      size = 0;
    RomState      state = RomState::Absent;
    std::string   found_as;        // entry name, when it differs from `name`
    std::string   found_in;        // archive path, when another set's archive provided it
    // Set only when an inherited ROM was satisfied by an ancestor's archive
    // rather than the set's own : which set (parent or BIOS short name)
    // provided it. This is what keeps a split audit explainable: "present,
    // from mslug2" rather than a bare "present" that the set's own zip cannot
    // account for.
    std::string   inherited_from;
    bool          inherited = false;  // the DAT marks this ROM merge=
};

struct Verdict {
    std::string status;               // "available" | "incorrect" | "missing" | "" (no ROMs)
    std::vector<RomVerdict> roms;     // every non-nodump ROM, in DAT order
};

// `own` is the set's own archive (null when none was found : every ROM then
// counts as absent, save inherited ones an ancestor can still provide). In
// NonMerged the lookups are never called and the outcome is byte-for-byte the
// rule the scanner always applied.
Verdict evaluate(const Game& game, const Archive* own, SetStyle style,
                 const ArchiveLookup& archive_for, const GameLookup& game_for);

// Convenience: status only, same rule.
std::string status_of(const Game& game, const Archive* own, SetStyle style,
                      const ArchiveLookup& archive_for, const GameLookup& game_for);

// ── The zip_contents cache as a source of archives ──────────────────────────
//
// Built once from DatabaseManager::getAllZipContents(); indexes every cached
// archive by the lower-cased stem of its filename. Several archives can share
// a stem (the same short name under two systems, or an outbox someone added to
// roms_paths next to the library) : `for_game` scores each candidate by how
// many of the set's own ROMs it satisfies and takes the best, breaking a tie
// by the folder whose name matches the set's DAT header. Same tie-break as the
// audit, so the two never claim different archives for the same set.
class CacheIndex {
public:
    // `roots` restricts the index to archives under the configured ROM
    // directories; empty accepts everything the cache knows. Archives that
    // no longer exist on disk are skipped either way.
    CacheIndex(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roots);

    const Archive* for_game(const Game& game) const;
    const Archive* by_path(const std::string& path) const;
    const std::unordered_map<std::string, Archive>& all() const { return m_archives; }
    size_t         size() const { return m_archives.size(); }
    bool           empty() const { return m_archives.empty(); }

private:
    std::unordered_map<std::string, Archive>                  m_archives;   // path → contents
    std::unordered_map<std::string, std::vector<std::string>> m_by_stem;    // lower stem → paths
};

// Re-derive the status of every set that inherits at least one ROM (style
// Split), from the cache alone : no disk I/O. Sets whose own archive is not
// in the cache are left untouched: the cache cannot say anything about them,
// and a stale cache must not turn a real set into a missing one.
//
// `touched` narrows the pass after an incremental scan: the lower-cased short
// names of the sets whose archives were just (re)read. A set is re-derived
// when its own name is in there, or any ancestor's is : a rescanned parent
// changes what its clones can inherit. Empty means every set.
//
// Returns the number of statuses that actually changed.
int resolve_inherited_from_cache(std::shared_ptr<DatabaseManager> db,
                                 const std::vector<std::string>& roots,
                                 SetStyle style,
                                 const std::unordered_set<std::string>& touched = {});

} // namespace RomResolve
