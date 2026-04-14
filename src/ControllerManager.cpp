// src/ControllerManager.cpp
#include "ControllerManager.h"
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <glob.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── Device discovery ───────────────────────────────────────────────────────

std::vector<JoystickInfo> ControllerManager::list_devices() {
    std::vector<JoystickInfo> result;
    glob_t g{};
    if (glob("/dev/input/js*", 0, nullptr, &g) != 0) return result;

    for (size_t i = 0; i < g.gl_pathc; ++i) {
        std::string path = g.gl_pathv[i];
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        JoystickInfo info;
        info.path = path;

        char name[256] = {};
        if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0)
            info.name = std::string(name);
        else
            info.name = "Joystick";

        uint8_t nb = 0, na = 0;
        ioctl(fd, JSIOCGBUTTONS, &nb);
        ioctl(fd, JSIOCGAXES,    &na);
        info.num_buttons = nb;
        info.num_axes    = na;

        close(fd);
        result.push_back(std::move(info));
    }
    globfree(&g);
    return result;
}

// ── Device I/O ────────────────────────────────────────────────────────────

int ControllerManager::open_device(const std::string& path) {
    return open(path.c_str(), O_RDONLY | O_NONBLOCK);
}

void ControllerManager::close_device(int fd) {
    if (fd >= 0) close(fd);
}

bool ControllerManager::poll_event(int fd, InputBinding& result) {
    if (fd < 0) return false;

    struct js_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        const uint8_t type = ev.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON && ev.value == 1) {
            result = { true, false, ev.number, -1, 0 };
            return true;
        }
        if (type == JS_EVENT_AXIS && std::abs(ev.value) > 16000) {
            result = { true, true, -1, ev.number, ev.value > 0 ? 1 : -1 };
            return true;
        }
    }
    return false;
}

// ── JSON helpers ──────────────────────────────────────────────────────────

static GameAction action_from_key(const std::string& k) {
    if (k == "up")      return GameAction::UP;
    if (k == "down")    return GameAction::DOWN;
    if (k == "left")    return GameAction::LEFT;
    if (k == "right")   return GameAction::RIGHT;
    if (k == "button1") return GameAction::BUTTON1;
    if (k == "button2") return GameAction::BUTTON2;
    if (k == "button3") return GameAction::BUTTON3;
    if (k == "button4") return GameAction::BUTTON4;
    if (k == "button5") return GameAction::BUTTON5;
    if (k == "button6") return GameAction::BUTTON6;
    if (k == "start")   return GameAction::START;
    if (k == "coin")    return GameAction::COIN;
    return GameAction::COUNT;
}

void ControllerManager::load_config(ControllerConfig& out, const std::string& config_path) {
    out = ControllerConfig();

    std::ifstream f(config_path);
    if (!f) return;

    try {
        json j;
        f >> j;
        if (!j.contains("controllers")) return;

        const auto& jctrl = j["controllers"];
        for (int p = 0; p < 2 && p < (int)jctrl.size(); ++p) {
            const auto& jp = jctrl[p];
            out.players[p].device_path = jp.value("device",      "");
            out.players[p].device_name = jp.value("device_name", "");

            if (!jp.contains("bindings")) continue;
            for (auto& [key, val] : jp["bindings"].items()) {
                GameAction action = action_from_key(key);
                if (action == GameAction::COUNT) continue;
                InputBinding b;
                b.valid    = true;
                b.is_axis  = val.value("is_axis", false);
                b.button   = val.value("button",  -1);
                b.axis     = val.value("axis",    -1);
                b.axis_dir = val.value("dir",      0);
                out.players[p].bindings[action] = b;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ControllerManager] load_config error: " << e.what() << std::endl;
    }
}

void ControllerManager::save_config(const ControllerConfig& cfg, const std::string& config_path) {
    // Read existing JSON to preserve all other keys
    json j;
    {
        std::ifstream fi(config_path);
        if (fi) { try { fi >> j; } catch (...) {} }
    }

    json jctrl = json::array();
    for (const auto& player : cfg.players) {
        json jp;
        jp["device"]      = player.device_path;
        jp["device_name"] = player.device_name;

        json jb = json::object();
        for (const auto& [action, binding] : player.bindings) {
            if (!binding.valid) continue;
            json jv;
            jv["is_axis"] = binding.is_axis;
            if (binding.is_axis) {
                jv["axis"] = binding.axis;
                jv["dir"]  = binding.axis_dir;
            } else {
                jv["button"] = binding.button;
            }
            jb[game_action_key(action)] = jv;
        }
        jp["bindings"] = jb;
        jctrl.push_back(jp);
    }
    j["controllers"] = jctrl;

    std::ofstream fo(config_path);
    if (fo) fo << j.dump(2) << std::endl;
}

// ── FBNeo config writer ───────────────────────────────────────────────────

std::string ControllerManager::get_fbneo_config_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/fbneo/config";
}

// Map GameAction index → FBNeo input name suffix (e.g. "up", "fire 1")
static const char* FBNEO_ACTION_NAMES[GAME_ACTION_COUNT] = {
    "up", "down", "left", "right",
    "fire 1", "fire 2", "fire 3", "fire 4", "fire 5", "fire 6",
    "start", "coin"
};

// FBNeo axis direction codes for UP/DOWN/LEFT/RIGHT (in GameAction order)
// FBNeo hardcodes: 0x00=left, 0x01=right, 0x02=up, 0x03=down
static const uint32_t FBNEO_AXIS_CODES[4] = {
    0x02, // UP
    0x03, // DOWN
    0x00, // LEFT
    0x01, // RIGHT
};

// Update or add a key=value line in fbneo.ini (key is the token before the first space)
static void upsert_ini_line(std::vector<std::string>& lines,
                            const std::string& key,
                            const std::string& new_line)
{
    for (auto& line : lines) {
        // Match lines that begin with the key followed by a space or end-of-string
        if (line.size() >= key.size()
            && line.compare(0, key.size(), key) == 0
            && (line.size() == key.size() || line[key.size()] == ' '))
        {
            line = new_line;
            return;
        }
    }
    lines.push_back(new_line);
}

// Read nIniVersion from fbneo.ini so we can stamp it into p?defaults.ini.
// FBNeo requires: nConfigMinVersion (0x020921) <= version <= nBurnVer
// Without a valid version line FBNeo skips all input entries.
static uint32_t read_fbneo_burn_ver(const std::string& fbneo_ini_path)
{
    std::ifstream f(fbneo_ini_path);
    if (!f) return 0x100000u; // safe fallback well above 0x020921

    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 12, "nIniVersion ") == 0) {
            try { return static_cast<uint32_t>(std::stoul(line.substr(12), nullptr, 0)); }
            catch (...) {}
        }
    }
    return 0x100000u;
}

void ControllerManager::write_fbneo_config(const ControllerConfig& cfg,
                                            const std::string& fbneo_config_dir)
{
    // Read the installed FBNeo version so we can stamp it in the defaults files.
    const uint32_t burn_ver = read_fbneo_burn_ver(fbneo_config_dir + "/fbneo.ini");

    for (int p = 0; p < 2; ++p) {
        const auto& player = cfg.players[p];
        if (player.device_path.empty() || player.bindings.empty()) continue;

        // Extract joystick index from path (e.g. /dev/input/js0 → 0, js12 → 12)
        int joy_index = 0;
        {
            const std::string& path = player.device_path;
            size_t i = path.size();
            while (i > 0 && std::isdigit((unsigned char)path[i - 1])) --i;
            if (i < path.size())
                joy_index = std::stoi(path.substr(i));
        }

        const uint32_t base = 0x4000u | (static_cast<uint32_t>(joy_index) << 8);

        // Write p(p+1)defaults.ini
        std::string ini_name = "p" + std::to_string(p + 1) + "defaults.ini";
        std::string ini_path = fbneo_config_dir + "/" + ini_name;

        std::ofstream f(ini_path);
        if (!f) {
            std::cerr << "[ControllerManager] Cannot write " << ini_path << std::endl;
            continue;
        }

        // Version header — required by FBNeo: nConfigMinVersion (0x020921) <= ver <= nBurnVer
        f << "version 0x" << std::uppercase << std::hex << burn_ver << "\n\n";

        const std::string prefix = "p" + std::to_string(p + 1) + " ";

        for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
            GameAction action = static_cast<GameAction>(a);
            auto it = player.bindings.find(action);
            if (it == player.bindings.end() || !it->second.valid) continue;

            const InputBinding& b = it->second;
            uint32_t sw = 0;

            if (b.is_axis) {
                if (a < 4) {
                    // UP/DOWN/LEFT/RIGHT → FBNeo hardcoded axis direction codes
                    sw = base | FBNEO_AXIS_CODES[a];
                } else {
                    // Non-directional axis binding — skip (FBNeo doesn't support it this way)
                    continue;
                }
            } else {
                sw = base | 0x80u | static_cast<uint32_t>(b.button);
            }

            std::ostringstream oss;
            oss << "input \"" << prefix << FBNEO_ACTION_NAMES[a]
                << "\" switch 0x"
                << std::uppercase << std::hex << sw;
            f << oss.str() << "\n";
        }

        std::cout << "[ControllerManager] Wrote " << ini_path << "\n";
    }

    // ── Update fbneo.ini szPlayerDefaultIni entries ───────────────────────
    std::string fbneo_ini = fbneo_config_dir + "/fbneo.ini";

    std::vector<std::string> lines;
    {
        std::ifstream fi(fbneo_ini);
        if (fi) {
            std::string line;
            while (std::getline(fi, line)) lines.push_back(line);
        }
    }

    for (int p = 0; p < 2; ++p) {
        const auto& player = cfg.players[p];
        std::string key = "szPlayerDefaultIni[" + std::to_string(p) + "]";

        std::string new_line;
        if (!player.device_path.empty() && !player.bindings.empty()) {
            new_line = key + " "
                       + fbneo_config_dir + "/p" + std::to_string(p + 1) + "defaults.ini";
        } else {
            new_line = key + " ";  // empty value → disabled
        }

        upsert_ini_line(lines, key, new_line);
    }

    std::ofstream fo(fbneo_ini);
    if (!fo) {
        std::cerr << "[ControllerManager] Cannot write " << fbneo_ini << "\n";
        return;
    }
    for (const auto& line : lines) fo << line << "\n";
    std::cout << "[ControllerManager] Updated " << fbneo_ini << "\n";
}
