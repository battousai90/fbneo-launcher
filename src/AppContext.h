// src/AppContext.h
#pragma once
#include <string>

class AppContext {
public:
    static std::string get_executable_dir();
    static std::string get_user_config_dir();
    static std::string get_config_path();

    // Root holding assets/ and locale/. Resolved once, by probing the layouts the
    // app can actually be run from: the build tree, a portable/AppImage bundle, and
    // an FHS install where the binary lands in /usr/bin while its data goes to
    // /usr/share/fbneo-launcher.
    static std::string get_data_dir();
    static std::string get_asset_path(const std::string& subpath);
    static std::string get_locale_dir();

private:
    static std::string s_executable_dir;
    static std::string s_data_dir;
};