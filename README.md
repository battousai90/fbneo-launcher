# fbneo-launcher

🎮 A lightweight, native GUI launcher for **FinalBurn Neo** on Linux.  
Built with **C++17** and **GTKmm 3.0**, designed to be fast, simple, and desktop-friendly.

No more terminal commands. Just select your game, tweak settings, and play.

![Screenshot mockup](assets/screenshot.png)

---

## ✨ Features

- 🕹️ Browse and launch ROMs with a clean interface
- ⚙️ Edit graphics settings (resolution, fullscreen, aspect ratio)
- 🎮 Configure input & controller presets
- 📁 Set custom paths: ROMs, saves, BIOS, snapshots
- 📄 Safe read/write of `fbneo.cfg`
- 🐧 100% native Linux app — no Electron, no web bloat
- 🛠️ Easy to build and run on Ubuntu/Debian

Perfect for retro enthusiasts who want a no-nonsense frontend.

---

## 🛠️ Build & Run

### 1. Install dependencies (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install -y build-essential g++ make cmake pkg-config libgtkmm-3.0-dev libpugixml-dev nlohmann-json3-dev libcurl4-openssl-dev