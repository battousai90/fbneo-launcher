// src/MainWindow.h
#pragma once

#include <gtkmm.h>
#include <string>
#include "Game.h"
#include "SettingsPanel.h"
#include "ModelColumns.h"
#include "ScanProgressDialog.h"
#include <atomic>

class MainWindow : public Gtk::Window {
public:
    MainWindow();
    virtual ~MainWindow();

private:
    // === Private Methods ===
    Glib::RefPtr<Gdk::Pixbuf> get_status_icon(const std::string& status);
    void on_game_selected();
    void on_play_clicked();
    void on_settings_clicked();
    void on_hide();
    void on_quit();
    
    // Menu handlers
    void on_export_game_list();
    void on_fbneo_menu();
    void on_video_settings();
    void on_audio_settings();
    void on_input_settings();
    void on_fullscreen_mode();
    void on_windowed_mode();
    void on_original_resolution();
    void on_arcade_mode();
    void on_console_mode();
    void on_all_systems();
    void on_rescan_roms();
    void on_verify_roms();
    void on_show_available_only();
    void on_show_missing_roms();
    void on_rom_info();
    void on_about_fbneo();
    void on_controls_help();
    void on_about_launcher();
    void save_scan_cache(const std::string& filename);
    bool load_scan_cache(const std::string& filename);
    void update_status_bar_stats();
    void on_start_scan_clicked();
    void update_fbneo_config(const std::vector<std::string>& roms_paths);
    void set_fbneo_system(const std::string& system);
    void on_system_filter_changed();
    void on_orientation_filter_changed();
    void on_driver_status_filter_changed();
    void on_rom_status_filter_changed();
    void filter_games();
    void configure_columns();
    std::string escape_markup(const std::string& text);

    // === Widgets ===
    SettingsPanel m_settings_panel;

    // === Menu Bar ===
    Gtk::MenuBar m_menu_bar;
    
    // File Menu
    Gtk::MenuItem m_menu_file;
    Gtk::Menu m_submenu_file;
    Gtk::MenuItem m_menu_item_settings;
    Gtk::MenuItem m_menu_item_export_game_list;
    Gtk::MenuItem m_menu_item_quit;
    
    // Emulator Menu
    Gtk::MenuItem m_menu_emulator;
    Gtk::Menu m_submenu_emulator;
    Gtk::MenuItem m_menu_item_fbneo_menu;
    Gtk::MenuItem m_menu_item_video_settings;
    Gtk::MenuItem m_menu_item_audio_settings;
    Gtk::MenuItem m_menu_item_input_settings;
    Gtk::MenuItem m_menu_item_fullscreen_mode;
    Gtk::MenuItem m_menu_item_windowed_mode;
    Gtk::MenuItem m_menu_item_original_resolution;
    
    // Systems Menu
    Gtk::MenuItem m_menu_systems;
    Gtk::Menu m_submenu_systems;
    Gtk::MenuItem m_menu_item_arcade_mode;
    Gtk::MenuItem m_menu_item_console_mode;
    Gtk::MenuItem m_menu_item_all_systems;
    
    // ROMs Menu
    Gtk::MenuItem m_menu_roms;
    Gtk::Menu m_submenu_roms;
    Gtk::MenuItem m_menu_item_rescan_roms;
    Gtk::MenuItem m_menu_item_verify_roms;
    Gtk::MenuItem m_menu_item_show_available_only;
    Gtk::MenuItem m_menu_item_show_missing_roms;
    Gtk::MenuItem m_menu_item_rom_info;
    
    // Help Menu
    Gtk::MenuItem m_menu_help;
    Gtk::Menu m_submenu_help;
    Gtk::MenuItem m_menu_item_about_fbneo;
    Gtk::MenuItem m_menu_item_controls_help;
    Gtk::MenuItem m_menu_item_about_launcher;

    // === Toolbar ===
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_toolbar_container{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_toolbar_row1{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_toolbar_row2{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button m_toolbar_play{"▶ Play"}; // Toolbar button to play selected game
    Gtk::Button m_button_scan{"Scan ROMs"}; // Button to scan for ROMs
    std::vector<Game> m_cached_games; // Cache for games
    Gtk::Entry m_search_entry; // Search entry for filtering games
    Gtk::ComboBoxText m_system_filter; // Filter by system type
    Gtk::ComboBoxText m_orientation_filter; // Filter by orientation  
    Gtk::ComboBoxText m_driver_status_filter; // Filter by driver status
    Gtk::ComboBoxText m_rom_status_filter; // Filter by ROM status (available/missing/incorrect)

    // === Status Bar ===
    Gtk::Box m_status_box{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Label m_status_label;
    Gtk::Box m_stats_box{Gtk::ORIENTATION_HORIZONTAL};

    // === Games List ===
    Gtk::ScrolledWindow m_scrolled_games;
    Gtk::TreeView m_treeview_games;
    Glib::RefPtr<Gtk::ListStore> m_model_games;
    ModelColumns m_columns;

    // === Game Details ===
    Gtk::Paned m_paned{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_details_box{Gtk::ORIENTATION_VERTICAL};
    Gtk::Image m_preview_image;
    Gtk::Label m_label_title;
    Gtk::Label m_label_info;
    Gtk::Button m_button_play{"▶ Launch"};
};