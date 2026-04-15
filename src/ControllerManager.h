// src/ControllerManager.h
#pragma once
#include "ControllerConfig.h"
#include <string>
#include <vector>
#include <map>

struct JoystickInfo {
    std::string path;        // /dev/input/js0
    std::string name;
    int num_buttons = 0;
    int num_axes    = 0;
};

class ControllerManager {
public:
    // Scan /dev/input/js* and return available joysticks
    static std::vector<JoystickInfo> list_devices();

    // Open a joystick device in non-blocking mode. Returns fd >= 0 or -1.
    static int  open_device(const std::string& path);
    static void close_device(int fd);

    // Non-blocking poll. Returns true and fills 'result' if a button press
    // or significant axis movement was detected.
    static bool poll_event(int fd, InputBinding& result);

    // ── Single-config persistence (legacy / backward-compat) ─────────────
    // Reads/writes only the "controllers" key in config_path.
    static void load_config(ControllerConfig& out,  const std::string& config_path);
    static void save_config(const ControllerConfig& cfg, const std::string& config_path);

    // ── Profile persistence ───────────────────────────────────────────────
    // "controller_profiles" + "active_controller_profile" keys in config_path.
    // On first call, migrates old "controllers" key → "Default" profile.
    static void load_profiles(std::map<std::string, ControllerConfig>& profiles,
                               std::string& active_name,
                               const std::string& config_path);

    static void save_profiles(const std::map<std::string, ControllerConfig>& profiles,
                               const std::string& active_name,
                               const std::string& config_path);

    // Write FBNeo per-player input defaults (p1defaults.ini / p2defaults.ini)
    // and update szPlayerDefaultIni entries in fbneo.ini.
    // fbneo_config_dir is typically ~/.local/share/fbneo/config
    static void write_fbneo_config(const ControllerConfig& cfg,
                                   const std::string& fbneo_config_dir);

    // Returns ~/.local/share/fbneo/config (does NOT create the directory)
    static std::string get_fbneo_config_dir();
};
