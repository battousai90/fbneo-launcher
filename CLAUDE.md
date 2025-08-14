# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System & Development Commands

This is a C++17/GTKmm 3.0 project using CMake. Key commands:

**Build:**
```bash
mkdir -p build && cd build
cmake ..
make
```

**Run:**
```bash
./build/fbneo-launcher
```

**Dependencies (Ubuntu/Debian):**
```bash
sudo apt install -y build-essential g++ make cmake pkg-config libgtkmm-3.0-dev libpugixml-dev nlohmann-json3-dev
```

**Additional runtime dependencies:** libzip-dev zlib1g-dev

## Architecture Overview

This is a GUI launcher for FinalBurn Neo (arcade emulator) with the following core architecture:

**Main Application Flow:**
- `main.cpp` → GTK Application → `MainWindow` (primary UI)
- `MainWindow` contains game list view + settings panel
- Games are loaded from DAT files and ROM directories are scanned
- Status tracking shows available/missing/incorrect ROM sets

**Key Components:**
- **MainWindow**: Primary UI with game list, play button, settings panel, status bar
- **Game/Rom structs**: Core data models for arcade games and ROM files
- **DatParser**: Parses FinalBurn Neo DAT files (XML format with game metadata)
- **RomScanner**: Scans ROM directories and validates against DAT CRC checksums
- **ScanCache**: Caches scan results to avoid re-scanning unchanged files
- **SettingsPanel**: Manages FinalBurn Neo configuration
- **IconManager**: Loads status icons (available/missing/error/incorrect)
- **AppContext**: Handles paths (config, cache, assets)

**Data Flow:**
1. DAT file parsed → Game database loaded
2. ROM directories scanned → ROM files validated against CRC
3. Game status updated (available/missing/incorrect)
4. UI reflects status with icons and filtering

**Key Files:**
- `data/*.dat` - FinalBurn Neo game databases
- `config.json` - Application settings
- `cache/scan_cache.json` - ROM scan cache
- `assets/icons/status-*.svg` - Status indicators

The application maintains separation between UI (GTKmm), data parsing (pugixml), and file operations (libzip), with threading for non-blocking ROM scanning.