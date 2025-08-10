// src/GameRow.h
#pragma once

#include <gtkmm.h>
#include <string>


class GameRow : public Gtk::ListBoxRow {
public:
    GameRow(const std::string& title, const std::string& rom_name,
            const std::string& manufacturer, const std::string& year);

    std::string get_rom_name() const { return m_rom_name; }
    std::string get_title() const { return m_title; }

private:
    std::string m_title;
    std::string m_rom_name;
    std::string m_manufacturer;
    std::string m_year;

    Gtk::Box m_box{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Label m_label_title;
    Gtk::Label m_label_info;
    Gtk::Image m_image;
};