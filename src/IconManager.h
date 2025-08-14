#pragma once
#include <gdkmm/pixbuf.h>

class IconManager {
public:
    static Glib::RefPtr<Gdk::Pixbuf> get_status_icon(const std::string& status);
};