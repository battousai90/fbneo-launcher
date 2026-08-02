// src/IconManager.cpp
#include "IconManager.h"
#include "AppContext.h"
#include <gdkmm/pixbuf.h>
#include <gtkmm.h>
#include <iostream>
#include <filesystem>
#include <mutex>
#include <unordered_map>

// Status icons are looked up once per game row during list refreshes. With
// 25k+ rows, reading and rescaling the SVG every call was the dominant
// "Not Responding" cause at startup and after scan. Cache them once.
static std::mutex g_icon_cache_mutex;
static std::unordered_map<std::string, Glib::RefPtr<Gdk::Pixbuf>> g_icon_cache;

Glib::RefPtr<Gdk::Pixbuf> IconManager::load(const std::string& subpath, int w, int h) {
    const std::string key = subpath + "@" + std::to_string(w) + "x" + std::to_string(h);
    {
        std::lock_guard<std::mutex> lock(g_icon_cache_mutex);
        auto it = g_icon_cache.find(key);
        if (it != g_icon_cache.end()) return it->second;
    }

    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    try {
        pixbuf = Gdk::Pixbuf::create_from_file(AppContext::get_asset_path(subpath), w, h);
    } catch (const Glib::Error& e) {
        std::cerr << "[WARN] icon not loaded: " << subpath << " (" << e.what() << ")" << std::endl;
    } catch (...) {
        std::cerr << "[WARN] icon not loaded: " << subpath << std::endl;
    }

    if (pixbuf) {
        std::lock_guard<std::mutex> lock(g_icon_cache_mutex);
        g_icon_cache.emplace(key, pixbuf);
    }
    return pixbuf;   // may be empty — Gtk::Image renders nothing, which is fine
}

Glib::RefPtr<Gdk::Pixbuf> IconManager::get_status_icon(const std::string& status) {
    {
        std::lock_guard<std::mutex> lock(g_icon_cache_mutex);
        auto it = g_icon_cache.find(status);
        if (it != g_icon_cache.end()) return it->second;
    }

    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    std::string icon_path = AppContext::get_asset_path("icons/status-" + status + ".svg");

    if (std::filesystem::exists(icon_path)) {
        try {
            auto raw = Gdk::Pixbuf::create_from_file(icon_path);
            if (raw) pixbuf = raw->scale_simple(24, 24, Gdk::INTERP_BILINEAR);
        } catch (...) {
            // Fall through to themed fallback below
        }
    }

    if (!pixbuf) {
        std::string fallback;
        if (status == "available")      fallback = "emblem-ok-symbolic";
        else if (status == "incorrect") fallback = "dialog-warning-symbolic";
        else if (status == "missing")   fallback = "image-missing-symbolic";
        else                            fallback = "dialog-error-symbolic";

        std::cout << "[INFO] Falling back to system icon: " << fallback << std::endl;
        try {
            pixbuf = Gtk::IconTheme::get_default()->load_icon(fallback, Gtk::ICON_SIZE_SMALL_TOOLBAR);
        } catch (...) {}
    }

    if (pixbuf) {
        std::lock_guard<std::mutex> lock(g_icon_cache_mutex);
        g_icon_cache.emplace(status, pixbuf);
    }
    return pixbuf;
}
