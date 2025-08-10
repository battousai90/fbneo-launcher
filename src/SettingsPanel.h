// src/SettingsPanel.h
#pragma once

#include <gtkmm.h>
#include <string>

class SettingsPanel : public Gtk::Box {
public:
    SettingsPanel();
    virtual ~SettingsPanel();

    std::string get_roms_path() const;
    void set_roms_path(const std::string& path);

    bool save_to_file(const std::string& filename = "config.json");
    bool load_from_file(const std::string& filename = "config.json");

private:
    void on_folder_clicked();

    Gtk::Box m_box{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Label m_label{"ROMs Directory:"};
    Gtk::Entry m_entry_path;
    Gtk::Button m_button_browse{"Browse..."};
};