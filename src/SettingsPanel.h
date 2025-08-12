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

    std::string get_dat_path() const;
    void set_dat_path(const std::string& path);

    std::string get_thumbnails_path() const;
    void set_thumbnails_path(const std::string& path);

    bool save_to_file(const std::string& filename = "config.json");
    bool load_from_file(const std::string& filename = "config.json");

    std::string get_fbneo_executable() const;
    void set_fbneo_executable(const std::string& path);

private:
    void on_folder_clicked(Gtk::Entry* entry);

    Gtk::Box m_box{Gtk::ORIENTATION_VERTICAL, 10};

    // Labels
    Gtk::Label m_label_roms{"ROMs Directory:"};
    Gtk::Label m_label_dat{"DAT File:"};
    Gtk::Label m_label_thumbs{"Thumbnails:"};
    Gtk::Label m_label_fbneo{"FBNeo Executable:"};

    // Entrées
    Gtk::Entry m_entry_roms;
    Gtk::Entry m_entry_dat;
    Gtk::Entry m_entry_thumbs;
    Gtk::Entry m_entry_fbneo;

    // Boutons
    Gtk::Button m_button_browse_roms{"Browse..."};
    Gtk::Button m_button_browse_dat{"Browse..."};
    Gtk::Button m_button_browse_thumbs{"Browse..."};
    Gtk::Button m_button_browse_fbneo{"Browse..."};
    Gtk::Button m_button_save{"Save"};
    Gtk::Button m_button_cancel{"Cancel"};
};