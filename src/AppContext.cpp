// src/AppContext.cpp
#include "AppContext.h"
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

// Static member initialization
std::string AppContext::s_executable_dir = "";

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
        if (system(("mkdir -p \"" + config_dir + "\"").c_str()) != 0) {
            std::cerr << "[ERROR] Failed to create config dir: " << config_dir << std::endl;
            return ".";
        }
        std::cout << "[INFO] Created config dir: " << config_dir << std::endl;
    }
    return config_dir;
}

std::string AppContext::get_config_path() {
    return get_user_config_dir() + "/config.json";
}

std::string AppContext::get_cache_path() {
    return get_user_config_dir() + "/scan_cache.json";
}

// ✅ Une seule définition
std::string AppContext::get_asset_path(const std::string& subpath) {
    return get_executable_dir() + "/assets/" + subpath;
}