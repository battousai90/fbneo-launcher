// src/RomCleanup.h
//
// Some library zips carry entries that no DAT rom for that set actually needs —
// leftovers from an older naming convention, stray files bundled by whoever
// built the archive, etc. (RomVault flags these as Purple/Brown: "not needed
// here"). FBNeo does not care that they are there — it only ever reads the
// entries it asks for by name — but they still take up disk space for nothing.
//
// RomAudit::audit() already finds them (GameEntry::extra_entries), using only
// the scan cache — no disk access. This module does the one thing that does
// touch disk: moving specific entries out of a specific archive.
#pragma once

#include "RomInbox.h"   // for RomInbox::Callbacks
#include <string>
#include <vector>

namespace RomCleanup {

// Extracts each listed entry from zip_path into
// quarantine_dir/_extra_files/<zip stem>/<entry name>, then removes it from
// the archive — "move, never destroy", the same philosophy as quarantining a
// whole set. Returns false if the zip could not be fully cleaned (see cb.log);
// entries already extracted before a failure are not rolled back.
bool extract_entries_to_quarantine(const std::string& zip_path,
                                    const std::vector<std::string>& entries,
                                    const std::string& quarantine_dir,
                                    const RomInbox::Callbacks& cb);

} // namespace RomCleanup
