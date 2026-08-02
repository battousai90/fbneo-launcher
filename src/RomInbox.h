// src/RomInbox.h
//
// Inbox → outbox ROM rebuilder, in the spirit of RomVault.
//
// The user drops downloaded archives into an *inbox*; analyze() recomputes every
// CRC32 in them, confronts them with the DAT-derived database, and produces a plan.
// apply() then writes the result into an *outbox* laid out like the DATs
// ("<outbox>/FinalBurn Neo - Arcade Games/mslug.zip").
//
// The existing ROM library is strictly READ-ONLY here: it only serves as a pool of
// pieces (via the zip_contents cache) from which incomplete sets can be rebuilt.
// Nothing in this module ever writes inside roms_paths, and nothing updates game
// statuses in the database — a rebuilt set becomes "available" only once the user
// points a normal scan at the outbox, which is the intended independent check.
#pragma once

#include "DatabaseManager.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RomInbox {

// Where one ROM piece can be read from. `container` is always a ZIP path.
struct PieceSource {
    std::string container;
    std::string entry;
    bool        from_inbox = false;
};

// One ROM of a DAT set, and where it will be taken from.
struct PiecePlan {
    std::string   target_name;   // DAT rom name = entry name in the produced ZIP
    unsigned long crc  = 0;
    uint64_t      size = 0;
    PieceSource   src;
    bool          resolved = false;
};

// A DAT ROM that could be found neither in the inbox nor in the library. Carries
// the DAT's own size/CRC so the report is enough to go hunting for the file.
struct MissingRom {
    std::string   name;
    unsigned long crc  = 0;
    uint64_t      size = 0;
};

enum class Action {
    Move,             // the inbox archive is already a perfect set: just relocate it
    Rebuild,          // every piece is available, but the ZIP must be recomposed
    Incomplete,       // at least one ROM is nowhere to be found
    AlreadyInLibrary, // the library already holds this set correctly; nothing to do
};

struct SetPlan {
    std::string game_name;
    std::string system;
    std::string dat_header;      // raw DAT header = outbox subfolder name
    std::string description;
    Action      action = Action::Incomplete;
    std::string dest_path;       // <outbox>/<dat_header>/<game_name>.zip
    std::string trigger_archive; // inbox archive that surfaced this set

    std::vector<PiecePlan>   pieces;
    std::vector<MissingRom>  missing;        // unresolved ROMs
    std::vector<std::string> extra_entries;  // entries of trigger_archive left out
    int  pieces_from_library = 0;            // how many pieces come from roms_paths
    int  renamed_entries     = 0;            // pieces whose source entry name differs
    bool selected = true;                    // UI checkbox
};

struct Report {
    std::vector<SetPlan>     sets;
    std::vector<std::string> unrecognized;  // inbox archives matching no DAT entry
    std::vector<std::string> unsupported;   // .7z / .rar / anything libzip refuses
    int complete = 0, fixable = 0, incomplete = 0, already = 0;
    bool cancelled = false;
    // True when the library pool was empty, which makes "Incomplete" verdicts
    // unreliable — the UI surfaces this as a "run a ROM scan first" hint.
    bool library_pool_empty = false;
};

// Progress/log/cancel plumbing, so the engine stays free of any GTK dependency.
struct Callbacks {
    std::function<void(double, const std::string&)> progress;  // percentage, message
    std::function<void(const std::string&)>         log;
    std::function<bool()>                           cancelled;
};

Report analyze(const std::string& inbox_dir,
               const std::string& outbox_dir,
               std::shared_ptr<DatabaseManager> db,
               bool recursive,
               const Callbacks& cb);

struct ApplyResult {
    int moved = 0, rebuilt = 0, skipped = 0, failed = 0;
    int consumed_archives = 0;
    std::vector<std::string> errors;
    bool cancelled = false;
};

ApplyResult apply(const Report& report, const Callbacks& cb);

// Outbox subfolder for a game: its raw DAT header when known, otherwise
// reconstructed from the trimmed system name. Exposed for the UI's preview column.
std::string outbox_subdir_for(const Game& game);

const char* action_label(Action a);

} // namespace RomInbox
