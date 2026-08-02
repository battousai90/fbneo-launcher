#pragma once
#include <gdkmm/pixbuf.h>
#include <string>

class IconManager {
public:
    static Glib::RefPtr<Gdk::Pixbuf> get_status_icon(const std::string& status);

    // Load an icon from the data directory, e.g. "icons/download.svg".
    //
    // Gdk::Pixbuf::create_from_file *throws* when the file is missing, and an
    // uncaught Glib::FileError in a widget constructor takes the whole
    // application down. A missing decorative icon should cost a button its
    // picture, nothing more — so this returns an empty RefPtr instead, which
    // Gtk::Image accepts happily.
    static Glib::RefPtr<Gdk::Pixbuf> load(const std::string& subpath, int w = 16, int h = 16);
};