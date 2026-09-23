// src/AppContext.cpp
#include "AppContext.h"
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <cstdlib>
#include <malloc.h>
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

// Anciens emplacements de l'etat utilisateur, dans l'ordre de preference.
// Le launcher s'est appele fbneo-launcher jusqu'en 1.3.x ; son dossier doit
// suivre le renommage sans que personne ne perde ses favoris ni son temps de
// jeu. C'est la seule donnee qui ne se reconstruit pas : games.db se regenere
// depuis les DAT, player_stats non.
static std::vector<std::string> legacy_config_dirs(const std::string& home) {
    std::vector<std::string> out;
    out.push_back(home + "/.config/fbneo-launcher");

    // Sous Flatpak, HOME vaut <vrai home>/.var/app/<app-id> : l'etat de
    // l'ancien app-id vit donc a cote, sous un autre <app-id>, hors de portee
    // d'un chemin simplement relatif a HOME.
    const std::string marker = "/.var/app/";
    const auto pos = home.rfind(marker);
    if (pos != std::string::npos) {
        out.push_back(home.substr(0, pos) +
                      "/.var/app/io.github.battousai90.FbneoLauncher"
                      "/config/fbneo-launcher");
    }
    return out;
}

// Deplace l'ancien dossier sur le nouveau. Un rename suffit quand les deux
// sont sur le meme systeme de fichiers ; sinon on copie (games.db pese ~150 Mo,
// donc on evite la copie quand on peut). En cas d'echec on rend l'ancien
// chemin plutot que de demarrer sur une base vide : mieux vaut un launcher qui
// tourne encore sur l'ancien etat qu'un launcher qui a l'air d'avoir tout
// perdu. L'ancien dossier n'est jamais supprime.
static bool migrate_config_dir(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(to).parent_path(), ec);

    ec.clear();
    std::filesystem::rename(from, to, ec);
    if (!ec) {
        std::cout << "[INFO] Migrated user data: " << from << " -> " << to << std::endl;
        return true;
    }

    ec.clear();
    std::filesystem::copy(from, to,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        std::cerr << "[WARN] Could not migrate " << from << " -> " << to
                  << " (" << ec.message() << "). Staying on the old directory."
                  << std::endl;
        // Pas de demi-migration : un dossier a moitie copie serait pris pour
        // un etat valide au prochain demarrage.
        std::error_code ignored;
        std::filesystem::remove_all(to, ignored);
        return false;
    }
    std::cout << "[INFO] Copied user data: " << from << " -> " << to
              << " (old directory kept as a backup)" << std::endl;
    return true;
}

std::string AppContext::get_user_config_dir() {
    // Un chantier parallele (le support MAME, une campagne de tests, une
    // capture de demo) doit pouvoir tourner sans toucher a l'etat du launcher
    // installe. Symetrique de BOOTCADE_DATA_DIR pour les assets.
    if (const char* env = std::getenv("BOOTCADE_CONFIG_DIR")) {
        if (*env) {
            std::error_code ec;
            std::filesystem::create_directories(env, ec);
            return env;
        }
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        std::cerr << "[ERROR] HOME not set. Using '.'" << std::endl;
        return ".";
    }

    const std::string config_dir = std::string(home) + "/.config/bootcade";
    std::error_code ec;
    if (std::filesystem::exists(config_dir, ec)) return config_dir;

    for (const auto& legacy : legacy_config_dirs(home)) {
        ec.clear();
        if (!std::filesystem::is_directory(legacy, ec)) continue;
        if (migrate_config_dir(legacy, config_dir)) return config_dir;
        return legacy;  // migration impossible : on continue sur l'ancien etat
    }

    ec.clear();
    if (!std::filesystem::create_directories(config_dir, ec)) {
        std::cerr << "[ERROR] Failed to create config dir: " << config_dir
                  << " (" << ec.message() << ")" << std::endl;
        return ".";
    }
    std::cout << "[INFO] Created config dir: " << config_dir << std::endl;
    return config_dir;
}

std::string AppContext::get_config_path() {
    return get_user_config_dir() + "/config.json";
}



// Where assets/ and locale/ live. The build tree keeps them next to the binary,
// but a packaged build installs the binary in <prefix>/bin and its data in
// <prefix>/share/bootcade, so the location has to be discovered rather than
// assumed. Probed once and cached.
std::string AppContext::get_data_dir() {
    if (!s_data_dir.empty()) return s_data_dir;

    const std::string exe_dir = get_executable_dir();
    std::vector<std::string> candidates;

    // An explicit override always wins : handy for testing an uninstalled build.
    if (const char* env = std::getenv("BOOTCADE_DATA_DIR"))
        if (*env) candidates.emplace_back(env);

    candidates.push_back(exe_dir);                                    // build tree / portable
    candidates.push_back(exe_dir + "/../share/bootcade");       // /usr/bin -> /usr/share
#ifdef BOOTCADE_INSTALL_DATA_DIR
    candidates.push_back(BOOTCADE_INSTALL_DATA_DIR);                             // baked in at configure time
#endif
    candidates.push_back("/usr/share/bootcade");
    candidates.push_back("/usr/local/share/bootcade");
    candidates.push_back("/app/share/bootcade");                // Flatpak prefix

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

void AppContext::trim_heap() {
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

std::string AppContext::get_locale_dir() {
    return get_data_dir() + "/locale";
}
bool AppContext::in_flatpak() {
    // Flatpak always creates this file inside the sandbox.
    static const bool yes = std::filesystem::exists("/.flatpak-info");
    return yes;
}

std::vector<std::string> AppContext::host_command(const std::vector<std::string>& args) {
    if (!in_flatpak() || args.empty()) return args;
    std::vector<std::string> out{"flatpak-spawn", "--host"};
    out.insert(out.end(), args.begin(), args.end());
    return out;
}
