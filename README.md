# fbneo-launcher

🎮 A lightweight, native GUI launcher for **FinalBurn Neo** on Linux.
Built with **C++17** and **GTKmm 3.0**, designed to be fast, simple, and desktop-friendly.

No more terminal commands. Scan your ROMs, pick a game, and play.

---

## ✨ Features

### Library
- 🕹️ Browse and launch your collection in a **list** or **cover grid** view
- 🔍 Instant search across titles, ROM names, manufacturers and years
- ⭐ Mark games as favorites and filter to them from the header
- 🧩 **Stackable filters** — combine system, type, year, manufacturer, status…
  (e.g. *Arcade + Original*, *SNES + Homebrew*), with active filters shown as
  removable chips
- 🏷️ **Type filter** — tell real games apart from derivatives: Original, Clone,
  Hack, Homebrew, Bootleg, Prototype
- 📊 Sidebar filters by system, manufacturer, year, source, aspect ratio,
  orientation and ROM status
- ⚡ Rows are built lazily as you scroll, so a 25k-game library opens instantly

### ROM management
- 📀 **ROM scanning with CRC verification** against FinalBurn Neo DAT files
- 🗃️ Local **SQLite** database — incremental rescans only re-read what changed
  on disk
- 📥 Download the latest FBNeo release, generate or update DAT files
- 🖼️ Fetch preview and title artwork automatically
- 📤 Export your game list

### Launching
- 🚀 Launch games straight into FBNeo, with joystick support always enabled
- 🖥️ Optional `-fullscreen` and `-integerscale` toggles
- 🎮 Controller configuration with reusable profiles

### App
- 🌍 Available in English, French, Spanish, German, Portuguese, Japanese and
  Chinese
- 🌑 Dark, modern interface
- 🐧 100% native Linux app — no Electron, no web bloat

Perfect for retro enthusiasts who want a no-nonsense frontend.

---

## 🛠️ Build & Run

### 1. Install dependencies (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential g++ make cmake pkg-config \
  libgtkmm-3.0-dev libpugixml-dev nlohmann-json3-dev \
  libcurl4-openssl-dev libzip-dev zlib1g-dev libsqlite3-dev
```

### 2. Build

```bash
cmake -B build
cmake --build build -j$(nproc)
```

### 3. Run

```bash
./build/fbneo-launcher
```

The build copies `assets/` and `locale/` next to the binary, so run it from
`build/` (or ship that directory as a whole).

---

## 🚀 First run

1. Open **File → Launcher Settings…**
2. Point the launcher at your **FinalBurn Neo executable**
3. Add one or more **ROM directories**
4. Set the **DAT directory** — DATs describe every known set and their CRCs, and
   are what the scanner checks your ROMs against. You can generate them from
   **Emulator → Generate DAT Files**, or fetch a build via **Download Latest
   FBNeo Release**
5. Hit **Scan ROMs**

Your library, settings and cache live in `~/.config/fbneo-launcher/`.

Rescans are incremental: only directories whose contents actually changed are
re-read, so a rebuilt ROM set is picked up without a full re-scan of the
collection.
