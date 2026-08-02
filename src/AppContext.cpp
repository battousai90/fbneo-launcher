// src/AppContext.cpp
#include "AppContext.h"
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

// Static member initialization
std::string AppContext::s_executable_dir = "";
std::string AppContext::s_data_dir = "";

std::string AppContext::get_executable_dir() {
    if (!s_executable_dir.empty()) {
        return s_executable_dir;
    }

    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count != -1) {
        s_executable_dir.assign(result, (count > 0) ? count : 0);
        s_executable_dir = std::filesystem::path(s_executable_dir).parent_path().string();
        std::cout << "[DEBUG] Executable directory cached: " << s_executable_dir << std::endl;
        return s_executable_dir;
    }

    std::cerr << "[ERROR] Failed to read /proc/self/exe. Using '.'" << std::endl;
    s_executable_dir = ".";
    return s_executable_dir;
}

std::string AppContext::get_user_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        std::cerr << "[ERROR] HOME not set. Using '.'" << std::endl;
        return ".";
    }

    std::string config_dir = std::string(home) + "/.config/fbneo-launcher";
    if (!std::filesystem::exists(config_dir)) {
        std::error_code ec;
        if (!std::filesystem::create_directories(config_dir, ec)) {
            std::cerr << "[ERROR] Failed to create config dir: " << config_dir
                      << " (" << ec.message() << ")" << std::endl;
            return ".";
        }
        std::cout << "[INFO] Created config dir: " << config_dir << std::endl;
    }
    return config_dir;
}

std::string AppContext::get_config_path() {
    return get_user_config_dir() + "/config.json";
}



// Where assets/ and locale/ live. The build tree keeps them next to the binary,
// but a packaged build installs the binary in <prefix>/bin and its data in
// <prefix>/share/fbneo-launcher, so the location has to be discovered rather than
// assumed. Probed once and cached.
std::string AppContext::get_data_dir() {
    if (!s_data_dir.empty()) return s_data_dir;

    const std::string exe_dir = get_executable_dir();
    std::vector<std::string> candidates;

    // An explicit override always wins — handy for testing an uninstalled build.
    if (const char* env = std::getenv("FBNEO_LAUNCHER_DATA_DIR"))
        if (*env) candidates.emplace_back(env);

    candidates.push_back(exe_dir);                                    // build tree / portable
    candidates.push_back(exe_dir + "/../share/fbneo-launcher");       // /usr/bin -> /usr/share
#ifdef FBNEO_DATA_DIR
    candidates.push_back(FBNEO_DATA_DIR);                             // baked in at configure time
#endif
    candidates.push_back("/usr/share/fbneo-launcher");
    candidates.push_back("/usr/local/share/fbneo-launcher");
    candidates.push_back("/app/share/fbneo-launcher");                // Flatpak prefix

    std::error_code ec;
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        if (std::filesystem::is_directory(c + "/assets", ec)) {
            s_data_dir = std::filesystem::weakly_canonical(c, ec).string();
            if (ec) s_data_dir = c;
            std::cout << "[DEBUG] Data directory: " << s_data_dir << std::endl;
            return s_data_dir;
        }
    }

    std::cerr << "[WARN] No assets directory found; falling back to " << exe_dir << std::endl;
    s_data_dir = exe_dir;
    return s_data_dir;
}

std::string AppContext::get_asset_path(const std::string& subpath) {
    return get_data_dir() + "/assets/" + subpath;
}

std::string AppContext::get_locale_dir() {
    return get_data_dir() + "/locale";
}