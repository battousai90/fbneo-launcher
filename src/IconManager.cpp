// src/IconManager.cpp
#include "IconManager.h"
#include "AppContext.h"
#include <gdkmm/pixbuf.h>
#include <gtkmm.h>
#include <iostream>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <gdkmm/pixbufloader.h>
#include <gdkmm/rgba.h>

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
    return pixbuf;   // may be empty : Gtk::Image renders nothing, which is fine
}

namespace {
// The SVG text and whether white is its only colour, read once per file.
struct SvgSource { std::string text; bool mono = false; };
std::unordered_map<std::string, SvgSource> g_svg_sources;

const SvgSource& svg_source(const std::string& subpath) {
    std::lock_guard<std::mutex> lock(g_icon_cache_mutex);
    auto it = g_svg_sources.find(subpath);
    if (it != g_svg_sources.end()) return it->second;
    SvgSource src;
    try {
        std::ifstream in(AppContext::get_asset_path(subpath));
        std::stringstream ss; ss << in.rdbuf();
        src.text = ss.str();
    } catch (...) {}
    // Every colour literal in the file must be white (or "none") for the
    // icon to count as monochrome. Named colours other than white, or any
    // other hex, mean the drawing carries its own palette.
    bool any = false, other = false;
    std::string t = src.text;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '#') {
            size_t j = i + 1;
            while (j < t.size() && std::isxdigit((unsigned char)t[j])) ++j;
            const std::string hex = t.substr(i + 1, j - i - 1);
            if (hex.size() == 3 || hex.size() == 6 || hex.size() == 8) {
                any = true;
                if (!(hex == "fff" || hex == "ffffff" || hex.rfind("ffffff", 0) == 0)) other = true;
            }
            i = j;
        }
    }
    if (t.find("\"white\"") != std::string::npos) any = true;
    if (t.find("currentcolor") != std::string::npos) other = true;
    src.mono = any && !other;
    return g_svg_sources.emplace(subpath, std::move(src)).first->second;
}

std::string hex_of(const Gdk::RGBA& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                  (int)std::lround(c.get_red() * 255), (int)std::lround(c.get_green() * 255), (int)std::lround(c.get_blue() * 255));
    return buf;
}
} // namespace

bool IconManager::is_monochrome(const std::string& subpath) {
    return svg_source(subpath).mono;
}

Glib::RefPtr<Gdk::Pixbuf> IconManager::load_tinted(const std::string& subpath, int w, int h, const Gdk::RGBA& colour) {
    const SvgSource& src = svg_source(subpath);
    if (!src.mono || src.text.empty()) return load(subpath, w, h);

    const std::string hex = hex_of(colour);
    const std::string key = subpath + "@" + std::to_string(w) + "x" + std::to_string(h) + hex + "/" + std::to_string((int)(colour.get_alpha() * 100));
    {
        std::lock_guard<std::mutex> lock(g_icon_cache_mutex);
        auto it = g_icon_cache.find(key);
        if (it != g_icon_cache.end()) return it->second;
    }
    // Substitute the white, keep everything else (geometry, stroke widths).
    // One pass over the hex literals : a naive "#fff" replacement inside a
    // "#ffffff" produced a nine-digit colour, which renders black.
    std::string svg;
    svg.reserve(src.text.size() + 16);
    for (size_t i = 0; i < src.text.size(); ++i) {
        if (src.text[i] == '#') {
            size_t j = i + 1;
            while (j < src.text.size() && std::isxdigit((unsigned char)src.text[j])) ++j;
            std::string run = src.text.substr(i + 1, j - i - 1);
            std::transform(run.begin(), run.end(), run.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (run == "fff" || run == "ffffff") { svg += hex; i = j - 1; continue; }
        }
        svg += src.text[i];
    }
    for (size_t pos = svg.find("\"white\""); pos != std::string::npos; pos = svg.find("\"white\"", pos + hex.size() + 2))
        svg.replace(pos, 7, "\"" + hex + "\"");
    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    try {
        auto loader = Gdk::PixbufLoader::create("svg");
        loader->set_size(w, h);
        loader->write(reinterpret_cast<const guint8*>(svg.data()), svg.size());
        loader->close();
        pixbuf = loader->get_pixbuf();
    } catch (const Glib::Error& e) {
        std::cerr << "[WARN] tinted icon not rendered: " << subpath << " (" << e.what() << ")" << std::endl;
        return load(subpath, w, h);
    }
    // A translucent ink (a theme's muted text may carry an alpha) : scale the
    // alpha channel ourselves, the SVG renderers disagree on a root opacity.
    if (pixbuf && colour.get_alpha() < 0.999 && pixbuf->get_has_alpha()) {
        pixbuf = pixbuf->copy();
        const double a = colour.get_alpha();
        guint8* px = pixbuf->get_pixels();
        const int rs = pixbuf->get_rowstride(), n = pixbuf->get_n_channels();
        for (int y = 0; y < pixbuf->get_height(); ++y)
            for (int x = 0; x < pixbuf->get_width(); ++x)
                px[y * rs + x * n + 3] = (guint8)std::lround(px[y * rs + x * n + 3] * a);
    }
    if (pixbuf) {
        std::lock_guard<std::mutex> lock(g_icon_cache_mutex);
        g_icon_cache.emplace(key, pixbuf);
    }
    return pixbuf;
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
