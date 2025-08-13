// src/IconManager.cpp
#include "IconManager.h"
#include "AppContext.h"
#include <gdkmm/pixbuf.h>
#include <gtkmm.h>
#include <iostream>
#include <filesystem>

Glib::RefPtr<Gdk::Pixbuf> IconManager::get_status_icon(const std::string& status) {
    std::cout << "[DEBUG] IconManager::get_status_icon(" << status << ")" << std::endl;

    std::string icon_path = AppContext::get_asset_path("icons/status-" + status + ".svg");
    std::cout << "[DEBUG] Trying to load: " << icon_path << std::endl;

    if (std::filesystem::exists(icon_path)) {
        try {
            auto pixbuf = Gdk::Pixbuf::create_from_file(icon_path);
            if (pixbuf) {
                std::cout << "[INFO] Loaded SVG icon: " << icon_path << std::endl;
                return pixbuf->scale_simple(24, 24, Gdk::INTERP_BILINEAR);
            }
        } catch (const Glib::FileError& e) {
            std::cerr << "[EXCEPTION] Glib::FileError: " << e.what() << std::endl;
        } catch (const Gdk::PixbufError& e) {
            std::cerr << "[EXCEPTION] Gdk::PixbufError: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[EXCEPTION] Unknown error loading SVG" << std::endl;
        }
    } else {
        std::cerr << "[ERROR] Icon file not found: " << icon_path << std::endl;
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