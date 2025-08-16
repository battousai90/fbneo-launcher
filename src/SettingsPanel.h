// src/SettingsPanel.h
#pragma once

#include <gtkmm.h>
#include <string>

class SettingsPanel : public Gtk::Box {
public:
    SettingsPanel();
    virtual ~SettingsPanel();

    std::string get_roms_path() const;  // Deprecated - returns first path for compatibility
    void set_roms_path(const std::string& path);  // Deprecated - sets first path for compatibility
    
    std::vector<std::string> get_roms_paths() const;
    void set_roms_paths(const std::vector<std::string>& paths);
    void add_roms_path(const std::string& path);
    void remove_roms_path(int index);

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
    void on_add_roms_path_clicked();
    void on_remove_roms_path_clicked();
    void refresh_roms_list();

    Gtk::Box m_box{Gtk::ORIENTATION_VERTICAL, 10};

    // Labels
    Gtk::Label m_label_roms{"ROMs Directories:"};
    Gtk::Label m_label_dat{"DAT Files Directory:"};
    Gtk::Label m_label_thumbs{"Thumbnails:"};
    Gtk::Label m_label_fbneo{"FBNeo Executable:"};

    // ROMs paths management
    std::vector<std::string> m_roms_paths;
    Gtk::ScrolledWindow m_scrolled_roms;
    Gtk::TreeView m_treeview_roms;
    Glib::RefPtr<Gtk::ListStore> m_model_roms;
    Gtk::TreeModel::ColumnRecord m_columns_roms;
    Gtk::TreeModelColumn<Glib::ustring> m_col_path;
    Gtk::Button m_button_add_roms{"Add Directory"};
    Gtk::Button m_button_remove_roms{"Remove Selected"};

    // Other entries
    Gtk::Entry m_entry_dat;
    Gtk::Entry m_entry_thumbs;
    Gtk::Entry m_entry_fbneo;

    // Boutons
    Gtk::Button m_button_browse_dat{"Browse..."};
    Gtk::Button m_button_browse_thumbs{"Browse..."};
    Gtk::Button m_button_browse_fbneo{"Browse..."};
    Gtk::Button m_button_save{"Save"};
    Gtk::Button m_button_cancel{"Cancel"};
};