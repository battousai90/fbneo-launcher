// src/i18n.cpp
#include "i18n.h"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstdlib>

namespace {
    std::unordered_map<std::string, std::string> g_catalog;
    std::string g_lang = "en";
    std::string g_dir;

    // Derive a 2-letter language code from the environment, e.g. "fr_FR.UTF-8" -> "fr".
    std::string detect_lang() {
        const char* vars[] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
        for (const char* v : vars) {
            const char* val = std::getenv(v);
            if (!val || !*val) continue;
            std::string s(val);
            if (auto colon = s.find(':'); colon != std::string::npos) s = s.substr(0, colon); // LANGUAGE list
            if (auto sep = s.find_first_of("_.@"); sep != std::string::npos) s = s.substr(0, sep);
            if (!s.empty() && s != "C" && s != "POSIX") return s;
        }
        return "en";
    }

    void load_catalog(const std::string& lang) {
        g_catalog.clear();
        if (lang == "en") return; // source language has no catalog

        std::string path = g_dir + "/" + lang + ".json";
        std::ifstream f(path);
        if (!f) {
            std::cerr << "[i18n] no catalog for '" << lang << "' (" << path << "), using English" << std::endl;
            return;
        }
        try {
            nlohmann::json j;
            f >> j;
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_string())
                    g_catalog[it.key()] = it.value().get<std::string>();
            std::cout << "[i18n] loaded " << g_catalog.size() << " strings for '" << lang << "'" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[i18n] parse error in " << path << ": " << e.what() << std::endl;
        }
    }
}

namespace i18n {
    void init(const std::string& locale_dir, const std::string& lang) {
        g_dir = locale_dir;
        g_lang = lang.empty() ? detect_lang() : lang;
        load_catalog(g_lang);
    }

    void set_language(const std::string& lang) {
        g_lang = lang;
        load_catalog(lang);
    }

    const std::string& language() { return g_lang; }

    std::string tr(const std::string& key) {
        auto it = g_catalog.find(key);
        return it != g_catalog.end() ? it->second : key;
    }

    std::vector<std::string> available_languages() {
        std::vector<std::string> out{"en"};
        try {
            for (const auto& e : std::filesystem::directory_iterator(g_dir))
                if (e.is_regular_file() && e.path().extension() == ".json")
                    out.push_back(e.path().stem().string());
        } catch (...) {}
        return out;
    }
}
