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
#include "RomScanner.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RomAudit {

enum class RomState {
    Present,    // right name, right CRC, in the set's own archive
    Corrupt,    // the file is there under the right name, but the data is wrong
    WrongName,  // right data in the archive, stored under another name
    Absent,     // not in the archive at all
};

struct RomEntry {
    std::string   name;
    unsigned long crc  = 0;
    uint64_t      size = 0;
    RomState      state = RomState::Absent;
    std::string   found_as;   // entry name, when state == WrongName
    std::string   found_in;   // another library archive holding this CRC, if any
};

struct GameEntry {
    std::string name, system, description, dat_header;
    std::string archive;              // archive that should hold the set
    bool        archive_found = false;
    std::string status;               // "available" | "incorrect" | "missing"
    std::vector<RomEntry> roms;
    int  absent = 0, wrong = 0, corrupt = 0;
    // Every absent ROM exists elsewhere in the library, so the set can be
    // reassembled locally rather than re-downloaded.
    bool repairable = false;
};

struct Report {
    std::vector<GameEntry> games;   // problem sets (or all, per `problems_only`)
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
