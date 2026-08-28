// src/SystemPrefix.h
#pragma once
#include <string>

// FBNeo prefixes its internal driver names for every non-arcade system so
// multiple consoles can share one namespace (e.g. "gba_007eon", "nes_smb"),
// but the DAT-derived ROM name stored in this app's database, and every image
// filename this app writes or looks up (thumbnails, F6 captures), always use
// the bare, unprefixed name. This is the one canonical mapping from the
// human-readable system name stored in the database to that prefix — every
// place that needs to reconstruct the FBNeo driver/file name from a bare ROM
// name must go through this, not a hand-rolled copy.
std::string get_fbneo_system_prefix(const std::string& game_system);
