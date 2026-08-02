# fbneo-launcher

🎮 A lightweight, native GUI launcher for **FinalBurn Neo** on Linux.
Built with **C++17** and **GTKmm 3.0**, designed to be fast, simple, and desktop-friendly.

No more terminal commands. Scan your ROMs, pick a game, and play.

**[Website](https://fbneo-launcher.netlify.app)** ·
**[Download](https://github.com/battousai90/fbneo-launcher/releases/latest)**

---

## 📦 Install

Grab a package from the [latest release](https://github.com/battousai90/fbneo-launcher/releases/latest):

| Format | For | Notes |
|---|---|---|
| **`.AppImage`** | any distribution | Portable, nothing to install. `chmod +x` then run. **Recommended.** |
| **`.deb`** | Debian / Ubuntu | `sudo apt install ./fbneo-launcher_*.deb` |
| **`.flatpak`** | any distribution | `flatpak install fbneo-launcher.flatpak` |
| **`.tar.gz`** | any distribution | Extract anywhere; system libraries required |

Each release ships a `SHA256SUMS` file: `sha256sum -c SHA256SUMS`.

You also need the FinalBurn Neo emulator itself — the launcher can fetch the
latest release for you from its Settings panel.

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
  libcurl4-openssl-dev libzip-dev libarchive-dev zlib1g-dev libsqlite3-dev
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
`build/` (or ship that directory as a whole). An installed build finds them in
`<prefix>/share/fbneo-launcher` instead; `FBNEO_LAUNCHER_DATA_DIR` overrides both.

### 4. Build the packages (optional)

```bash
./scripts/package.sh                 # everything it can build here
./scripts/package.sh deb tgz         # or just the ones you want
```

Artifacts land in `dist/`. Targets are `deb`, `tgz`, `appimage` and `flatpak`;
the last two need `curl` and `flatpak-builder` respectively, and are skipped with
a warning when unavailable.

This is the same script the release workflow runs, so what you build locally is
what users download. Pushing a tag publishes a release automatically:

```bash
git tag v1.0.0 && git push origin v1.0.0
```

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

### Testing the Flatpak locally

`flatpak-builder` is itself available as a Flatpak, so no root is needed:

```bash
flatpak install --user flathub org.flatpak.Builder
./scripts/package.sh flatpak          # produces dist/*.flatpak
flatpak install --user dist/fbneo-launcher-*.flatpak
flatpak run io.github.battousai90.FbneoLauncher
```

The first build is long: `org.gnome.Platform` ships GTK 3 but not the gtkmm C++
bindings, so libsigc++, glibmm, cairomm, pangomm, atkmm and gtkmm are compiled from
source. Later builds reuse the cache in `.flatpak-builder/`.

To uninstall: `flatpak uninstall --user io.github.battousai90.FbneoLauncher`.
