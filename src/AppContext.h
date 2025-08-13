// src/AppContext.h
#pragma once
#include <string>

class AppContext {
public:
    static std::string get_executable_dir();
    static std::string get_user_config_dir();
    static std::string get_config_path();
    static std::string get_cache_path();
    static std::string get_asset_path(const std::string& subpath);

private:
    static std::string s_executable_dir;
};