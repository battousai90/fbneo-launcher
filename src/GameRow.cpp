// src/GameRow.cpp
#include <iostream>
#include "GameRow.h"
#include <gdkmm/pixbuf.h>
#include <filesystem>


GameRow::GameRow(const std::string& title, const std::string& rom_name,
                 const std::string& manufacturer, const std::string& year, const std::string& status)
    : m_title(title), m_rom_name(rom_name),
      m_manufacturer(manufacturer), m_year(year), m_status(status)
{
    set_margin_top(5);
    set_margin_bottom(5);

    // --- image ---
    try {
        Glib::RefPtr<Gdk::Pixbuf> pixbuf = Gdk::Pixbuf::create_from_file("assets/thumbnails/" + rom_name + ".png");
        if (pixbuf) {
            // resize the image to fit the row
            auto scaled = pixbuf->scale_simple(64, 48, Gdk::INTERP_BILINEAR);
            m_image.set(scaled);
        } else {
            // default image if not found
            m_image.set_from_icon_name("image-missing", Gtk::ICON_SIZE_LARGE_TOOLBAR);
        }
    } catch (const Gdk::PixbufError& ex) {
        std::cout << "PixbufError: " << ex.what() << std::endl;
        m_image.set_from_icon_name("image-missing", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    } catch (const Glib::FileError& ex) {
        std::cout << "FileError: " << ex.what() << std::endl;
        m_image.set_from_icon_name("image-missing", Gtk::ICON_SIZE_LARGE_TOOLBAR);
    }

    m_image.set_margin_end(10);

    // --- labels ---
    m_label_title.set_text(title);
    m_label_title.set_halign(Gtk::ALIGN_START);
    m_label_info.set_text(manufacturer + " • " + year);
    m_label_info.get_style_context()->add_class("dim-label");

    // --- Layout ---
    m_box.set_margin_start(10);
    m_box.set_margin_end(10);
    m_box.pack_start(m_image, Gtk::PACK_SHRINK);
    m_box.pack_start(m_label_title);
    m_box.pack_start(m_label_info);

    m_box.show();
    add(m_box);
}