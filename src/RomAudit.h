// src/RomAudit.h
//
// Library-wide audit: answers "what exactly is wrong with my collection?".
//
// The scan already tells you *how many* sets are available / incorrect / missing.
// This tells you *which*, and down to the individual ROM: which file is absent,
// which one carries the right data under the wrong name, and whether a correct
// copy of a missing piece happens to exist somewhere else in the library (in which
// case the set is repairable without downloading anything).
//
// It reads the content-addressed cache (zip_contents) filled by the scan, so it
// costs no disk I/O and never writes anything.
#pragma once

#include "DatabaseManager.h"
#include "RomInbox.h"   // for RomInbox::Callbacks
#include "RomResolve.h"
#include "RomScanner.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RomAudit {

// The verdict on one ROM is RomResolve's : Present / WrongName / Corrupt /
// Absent, decided by the same rule the scanner applies.
using RomState = RomResolve::RomState;

struct RomEntry {
    std::string   name;
    unsigned long crc  = 0;
    uint64_t      size = 0;
    RomState      state = RomState::Absent;
    std::string   found_as;   // entry name, when it differs from `name`
    // Where the data actually is, when not in the set's own archive: the
    // ancestor's archive that provides an inherited ROM (Present/WrongName in
    // a split collection), or another library archive holding a good copy of
    // an Absent/Corrupt one : in which case the set is repairable locally.
    std::string   found_in;
    // The DAT marks this ROM merge= : it belongs to the parent or the BIOS.
    bool          inherited = false;
    // Set when an ancestor's archive satisfied it: that set's short name. An
    // audit that says "present" about a ROM the set's own zip does not hold
    // must be able to say who does.
    std::string   inherited_from;
};

struct GameEntry {
    std::string name, system, description, dat_header;
    std::string archive;              // archive that should hold the set
    bool        archive_found = false;
    std::string cloneof;              // parent's short name, empty if this is an original
    std::string status;               // "available" | "incorrect" | "missing"
    std::vector<RomEntry> roms;
    // Entries physically present in the archive but not required by any rom
    // above : a set can have these and still be "available" (they never stop
    // FBNeo from loading it), so they are tracked separately from the roms list.
    std::vector<std::string> extra_entries;
    int  absent = 0, wrong = 0, corrupt = 0;
    // Every absent ROM exists elsewhere in the library, so the set can be
    // reassembled locally rather than re-downloaded.
    bool repairable = false;
};

// One entry inside an archive that no current DAT game claims at all : as
// opposed to GameEntry::extra_entries, which lives inside an archive a real
// game DOES own. Matches RomVault's own two colours for this exact case:
// Brown ("not needed here, but a copy is located elsewhere") when some other
// archive holds the same data, Purple ("not needed here, and a copy isn't
// located elsewhere") when nothing else in the library has it either.
struct OrphanEntry {
    std::string   name;
    unsigned long crc = 0;
    bool          copy_elsewhere = false;
};

// A whole archive that matches no game in the current DAT by name or by
// content : typically a leftover from a driver FBNeo has since renamed or
// dropped. The whole file is what gets quarantined; the per-entry breakdown
// is purely informational (same reasoning as RomVault's Brown/Purple split).
struct OrphanArchive {
    std::string path;
    std::vector<OrphanEntry> entries;
};

struct Report {
    RomResolve::SetStyle style = RomResolve::SetStyle::NonMerged;  // the rule applied
    std::vector<GameEntry> games;   // problem sets (or all, per `problems_only`)
    std::vector<OrphanArchive> orphans; // archives no game in the DAT claims at all
    int  total = 0, available = 0, incorrect = 0, missing = 0;
    int  repairable = 0;
    bool cancelled = false;
    bool pool_empty = false;        // no scan cache: results would be meaningless
};

Report audit(std::shared_ptr<DatabaseManager> db,
             const std::vector<std::string>& roms_paths,
             bool problems_only,
             const RomInbox::Callbacks& cb);

} // namespace RomAudit
