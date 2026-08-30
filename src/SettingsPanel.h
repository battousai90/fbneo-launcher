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

    std::string get_previews_path() const;
    void set_previews_path(const std::string& path);
    
    std::string get_titles_path() const;
    void set_titles_path(const std::string& path);

    bool save_to_file(const std::string& filename = "config.json");
    bool load_from_file(const std::string& filename = "config.json");

    // Appearance / language
    std::string get_theme() const;               // "system" | "dark" | "light"
    void set_theme(const std::string& mode);
    std::string get_language() const;            // "" (system) | "en" | "fr" | ...
    void set_language(const std::string& code);
    sigc::signal<void, Glib::ustring>& signal_theme_changed()    { return m_sig_theme_changed; }
    sigc::signal<void, Glib::ustring>& signal_language_changed() { return m_sig_language_changed; }
    // Emis des que l'interrupteur bouge, pour que l'effet soit immediat.
    // Exiger un redemarrage pour un interrupteur serait une regression
    // deguisee en reglage.
    sigc::signal<void, bool>&          signal_hiscore_toggled()  { return m_sig_hiscore_toggled; }

    std::string get_fbneo_executable() const;
    void set_fbneo_executable(const std::string& path);

    // Public method for menu access
    void on_download_fbneo_clicked();
    void on_generate_dat_clicked();
    
    // Public access to download previews button
    Gtk::Button& get_download_previews_button() { return m_button_download_previews; }
    
    // Public access to download titles button  
    Gtk::Button& get_download_titles_button() { return m_button_download_titles; }
    
    // Public access to entry for menu
    Gtk::Entry m_entry_fbneo;
    // Scan options
    bool is_scan_recursive() const { return m_check_recursive.get_active(); }
    bool is_scan_loose_files() const { return m_check_loose_files.get_active(); }

    // ── Online scores ────────────────────────────────────────────────────
    // Nothing is ever sent without both of these: a name to sign with, and an
    // explicit yes. Sending a score also publishes how long someone played,
    // which is a habit, not a game statistic : it needs asking, not assuming.
    // Interrupteur unique : il gouverne tout, affichage comme envoi. Deux
    // réglages distincts, l'un pour lire et l'autre pour publier, étaient
    // redondants : personne n'active un classement en ligne pour le regarder
    // sans y figurer. Allumé, la pastille s'affiche et les scores partent ;
    // éteint, aucune pastille et aucune requête.
    bool        is_hiscore_enabled() const;
    std::string get_hiscore_player() const;
    std::string get_hiscore_country() const;

private:
    void on_folder_clicked(Gtk::Entry* entry);
    void on_add_roms_path_clicked();
    void on_remove_roms_path_clicked();
    void refresh_roms_list();
    void on_download_previews_clicked();
    void on_download_titles_clicked();

    Gtk::Box m_box{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Paned m_paned_roms{Gtk::ORIENTATION_VERTICAL};

    // Labels
    Gtk::Label m_label_roms{"ROMs Directories:"};
    Gtk::Label m_label_dat{"DAT Files Directory:"};
    Gtk::Label m_label_previews{"Previews:"};
    Gtk::Label m_label_titles{"Titles:"};
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
    Gtk::Entry m_entry_previews;
    Gtk::Entry m_entry_titles;

    // Boutons
    Gtk::Button m_button_browse_dat{"Browse..."};
    Gtk::Button m_button_browse_previews{"Browse..."};
    Gtk::Button m_button_download_previews{"Download All Previews"};
    Gtk::Button m_button_browse_titles{"Browse..."};
    Gtk::Button m_button_download_titles{"Download All Titles"};
    Gtk::Button m_button_browse_fbneo{"Select"};
    Gtk::Button m_button_download_fbneo{"Download"};
    Gtk::Button m_button_generate_dat{"Generate DAT"};
    // Scan options widgets
    Gtk::CheckButton m_check_recursive{"Scan directories recursively"};
    Gtk::CheckButton m_check_loose_files{"Include loose ROM files (non-zip)"};

    // Online scores
    Gtk::Label       m_label_hiscore_player{"Player name:"};
    Gtk::Entry       m_entry_hiscore_player;
    // Un vrai interrupteur, pas une case a cocher : c'est un etat marche /
    // arret pour toute une fonctionnalite, pas une option parmi d'autres.
    Gtk::Box    m_hiscore_row{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Label  m_label_hiscore_enabled;
    Gtk::Switch m_switch_hiscore;
    Gtk::Label m_label_hiscore_country{"Country:"};
    // A typed field with completion rather than a 249-row dropdown: scrolling
    // to your own country in an alphabetical list of every country on earth is
    // slower than typing three letters of it.
    Gtk::Entry m_entry_hiscore_country;
    struct CountryCols : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> code;
        CountryCols() { add(name); add(code); }
    };
    CountryCols                       m_country_cols;
    Glib::RefPtr<Gtk::ListStore>      m_country_model;
    Glib::RefPtr<Gtk::EntryCompletion> m_country_completion;
    void build_country_completion();
    void set_hiscore_country(const std::string& code);

    // Appearance / language
    Gtk::Label m_label_theme{"Theme:"};
    Gtk::Label m_label_language{"Language:"};
    Gtk::ComboBoxText m_combo_theme;
    Gtk::ComboBoxText m_combo_language;
    sigc::signal<void, Glib::ustring> m_sig_theme_changed;
    sigc::signal<void, Glib::ustring> m_sig_language_changed;
    sigc::signal<void, bool>          m_sig_hiscore_toggled;
    bool m_suppress_appearance_signals{false};
};