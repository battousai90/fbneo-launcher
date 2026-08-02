// src/AppContext.h
#pragma once
#include <string>
#include <vector>

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

    // True when running inside a Flatpak sandbox.
    static bool in_flatpak();

    // Prefix a command so it runs on the host when sandboxed, unchanged otherwise.
    //
    // FinalBurn Neo lives on the host and links against the host's libraries (SDL2
    // and friends), which do not exist inside the sandbox — executing it directly
    // from a Flatpak fails with "error while loading shared libraries". flatpak-spawn
    // hands it to the host instead; that is what the --talk-name=org.freedesktop.Flatpak
    // permission in the manifest exists for.
    static std::vector<std::string> host_command(const std::vector<std::string>& args);

private:
    static std::string s_executable_dir;
    static std::string s_data_dir;
};