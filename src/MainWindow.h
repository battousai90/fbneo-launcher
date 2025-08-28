// src/MainWindow.h
#pragma once

#include <gtkmm.h>
#include <string>
#include <thread>
#include "Game.h"
#include "SettingsPanel.h"
#include "ModelColumns.h"
#include "ROMScanDialog.h"
#include "ThumbnailDownloader.h"
#include "DatabaseManager.h"
#include "DATUpdateDialog.h"
#include "ConfirmationDialog.h"
#include "FilterCache.h"
#include <atomic>
#include <functional>
#include <memory>

class MainWindow : public Gtk::Window {
public:
    MainWindow(std::function<void(double, const std::string&)> progress_callback = nullptr, const std::vector<Game>& preloaded_games = {});
    virtual ~MainWindow();

private:
    // === Private Methods ===
    Glib::RefPtr<Gdk::Pixbuf> get_status_icon(const std::string& status);
    void on_game_selected();
    void on_play_clicked();
    void on_download_art_clicked();
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
    void on_download_latest_fbneo();
    void on_generate_dat_files();
    void update_status_bar_stats();
    void on_start_scan_clicked();
    
    // Thumbnail download methods
    void show_download_progress(const std::string& filename, int current, int total, double percentage);
    void hide_download_progress();
    void on_download_thumbnails_clicked();
    
    // ROM scan methods
    void on_scan_progress();
    void on_scan_finished();
    void start_scan_thread(const std::vector<std::string>& roms_paths);
    void on_update_dat_clicked();
    void update_fbneo_config(const std::vector<std::string>& roms_paths);
    void set_fbneo_system(const std::string& system);
    // Filter TreeView handlers
    void on_filter_selection_changed();
    void populate_filter_tree();
    void apply_tree_filters();
    void update_filter_counts();
    void filter_games();
    void filter_games_async();
    void filter_games_simple();
    void apply_filters();
    void load_filter_cache();
    void save_filter_cache();
    void configure_columns();
    std::string escape_markup(const std::string& text);
    Glib::RefPtr<Gdk::Pixbuf> get_filter_icon(const std::string& category);

    // === Widgets ===
    SettingsPanel m_settings_panel;
    
    // === Database ===
    std::shared_ptr<DatabaseManager> m_database;

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
    Gtk::MenuItem m_menu_item_download_latest_fbneo;
    Gtk::MenuItem m_menu_item_generate_dat_files;
    
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
    Gtk::MenuItem m_menu_item_update_dat;
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
    Gtk::Button m_button_update_dat{"🔄 Update DAT"}; // Button to update DAT database
    std::vector<Game> m_cached_games; // Cache for games (legacy, kept for compatibility)
    Gtk::Entry m_search_entry; // Search entry for filtering games
    // MAMEUI-style filter panel with TreeView
    Gtk::ScrolledWindow m_scrolled_filters;
    Gtk::TreeView m_treeview_filters;
    Glib::RefPtr<Gtk::TreeStore> m_model_filters;
    
    // Filter TreeView columns
    class FilterColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        FilterColumns() { add(m_col_icon); add(m_col_name); add(m_col_type); add(m_col_value); add(m_col_count); }
        Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> m_col_icon;
        Gtk::TreeModelColumn<Glib::ustring> m_col_name;
        Gtk::TreeModelColumn<Glib::ustring> m_col_type;  // "system", "manufacturer", etc.
        Gtk::TreeModelColumn<Glib::ustring> m_col_value; // actual filter value
        Gtk::TreeModelColumn<int> m_col_count; // number of games
    };
    FilterColumns m_filter_columns;
    
    // Current active filters
    std::map<std::string, std::string> m_active_filters;

    // === Status Bar ===
    Gtk::Box m_status_box{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Label m_status_label;
    Gtk::Box m_stats_box{Gtk::ORIENTATION_HORIZONTAL};
    
    // Thumbnail download progress
    Gtk::Box m_download_progress_box{Gtk::ORIENTATION_HORIZONTAL, 5};
    Gtk::Label m_download_status_label;
    Gtk::ProgressBar m_download_progress_bar;

    // === Games List ===
    Gtk::ScrolledWindow m_scrolled_games;
    Gtk::TreeView m_treeview_games;
    Glib::RefPtr<Gtk::ListStore> m_model_games;
    ModelColumns m_columns;

    // === 3-Panel Layout like MAMEUI ===
    Gtk::Paned m_paned_main{Gtk::ORIENTATION_HORIZONTAL}; // Filter panel | Rest
    Gtk::Paned m_paned_right{Gtk::ORIENTATION_HORIZONTAL}; // Game list | Details
    Gtk::Box m_details_box{Gtk::ORIENTATION_VERTICAL};
    Gtk::Image m_preview_image;
    Gtk::Label m_label_title;
    Gtk::Label m_label_info;
    Gtk::Button m_button_play{"▶ Launch"};
    Gtk::Button m_button_download_art{"🎨 Download Art"};
    
    // === Thumbnail Downloader ===
    ThumbnailDownloader m_thumbnail_downloader;
    
    // Threading pour progression thumbnails
    Glib::Dispatcher m_download_progress_dispatcher;
    Glib::Dispatcher m_download_finished_dispatcher;
    
    // Threading for ROM scan
    Glib::Dispatcher m_scan_progress_dispatcher;
    Glib::Dispatcher m_scan_finished_dispatcher;
    
    // Removed filter population threading
    
    // Variables partagées pour la progression (protégées par le dispatcher)
    std::string m_current_download_file;
    int m_current_download_index;
    int m_total_download_count;
    double m_download_percentage;
    
    // Variables for ROM scan progress (protected by dispatcher)
    std::atomic<bool> m_scan_cancelled{false};
    std::atomic<int> m_scan_current{0};
    std::atomic<int> m_scan_total{0};
    std::string m_current_scan_game;
    bool m_scan_in_progress = false;
    std::thread m_scan_thread;
    
    // Filter performance optimization
    sigc::connection m_search_timeout_connection;
    std::vector<Game> m_filtered_games;
    std::mutex m_filter_mutex;
    
    // Filter cache data
    FilterCache::FilterData m_filter_cache;
    bool m_filter_cache_loaded = false;
    
    // Removed filter population tracking
};