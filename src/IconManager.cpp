// src/IconManager.cpp
#include "IconManager.h"
#include "AppContext.h"
#include <gdkmm/pixbuf.h>
#include <gtkmm.h>
#include <iostream>
#include <filesystem>

Glib::RefPtr<Gdk::Pixbuf> IconManager::get_status_icon(const std::string& status) {
    std::string icon_path = AppContext::get_asset_path("icons/status-" + status + ".svg");

    if (std::filesystem::exists(icon_path)) {
        try {
            auto pixbuf = Gdk::Pixbuf::create_from_file(icon_path);
            if (pixbuf) {
                return pixbuf->scale_simple(24, 24, Gdk::INTERP_BILINEAR);
            }
        } catch (...) {
            // Silently fall through to default icon
        }
    }

    // Fallback
    std::string fallback;
    if (status == "available") fallback = "emblem-ok-symbolic";
    else if (status == "incorrect") fallback = "dialog-warning-symbolic";
    else if (status == "missing") fallback = "image-missing-symbolic";
    else fallback = "dialog-error-symbolic";

    std::cout << "[INFO] Falling back to system icon: " << fallback << std::endl;
    return Gtk::IconTheme::get_default()->load_icon(fallback, Gtk::ICON_SIZE_SMALL_TOOLBAR);
}