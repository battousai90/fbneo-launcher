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
#include "ControllerConfig.h"
#include "ControllerManager.h"
#include <atomic>
#include <functional>
#include <memory>
#include <map>

class MainWindow : public Gtk::Window {
public:
    MainWindow(std::shared_ptr<DatabaseManager> database,
               std::function<void(double, const std::string&)> progress_callback = nullptr,
               const std::vector<Game>& preloaded_games = {});
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
    void on_integerscale_mode();
    void on_arcade_mode();
    void on_console_mode();
    void on_all_systems();
    void on_rescan_roms();
    void on_verify_roms();
    void on_find_duplicate_roms();
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
    void on_scan_dialog_complete();
    void on_scan_go_background();
    bool on_scan_bg_poll();        // called by Glib::signal_timeout
    
    // Artwork download methods
    void show_download_progress(const std::string& filename, int current, int total, double percentage);
    void hide_download_progress();
    void on_download_previews_clicked();
    void on_download_titles_clicked();
    void on_download_cancel_clicked();
    
    // Settings dialog management
    sigc::signal<void> m_close_settings_signal;
    
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

    // === Header bar (modern titlebar) ===
    Gtk::HeaderBar    m_headerbar;
    Gtk::Button       m_btn_settings; // gear button in the header
    Gtk::MenuButton   m_menu_button;
    Gtk::Menu         m_app_menu;   // hamburger popup hosting the top-level menus
    Gtk::ComboBoxText m_lang_combo; // language selector shown in the header
    bool m_suppress_lang_signal{false};
    void populate_language_combo();
    void on_language_selected(const std::string& code);
    
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
    Gtk::CheckMenuItem m_menu_item_fullscreen_mode;
    Gtk::CheckMenuItem m_menu_item_integerscale_mode;
    Gtk::MenuItem m_menu_item_download_latest_fbneo;
    Gtk::MenuItem m_menu_item_generate_dat_files;
    
    // Filter Menu
    Gtk::MenuItem m_menu_filter;
    Gtk::Menu m_submenu_filter;
    Gtk::MenuItem m_menu_item_arcade_mode;
    Gtk::MenuItem m_menu_item_console_mode;
    Gtk::MenuItem m_menu_item_all_systems;
    Gtk::MenuItem m_menu_item_show_available_only;
    Gtk::MenuItem m_menu_item_show_missing_roms;
    
    // ROMs Menu
    Gtk::MenuItem m_menu_roms;
    Gtk::Menu m_submenu_roms;
    Gtk::MenuItem m_menu_item_rescan_roms;
    Gtk::MenuItem m_menu_item_update_dat;
    Gtk::MenuItem m_menu_item_verify_roms;
    Gtk::MenuItem m_menu_item_rom_info;
    Gtk::MenuItem m_menu_item_find_duplicates;
    
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
    Gtk::Box   m_status_box{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Label m_status_label;
    Gtk::Box   m_stats_box{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Label m_summary_label; // "N available / Total" on the right

    // Scan progress widgets (shown only while a scan is running)
    Gtk::Box         m_scan_status_box{Gtk::ORIENTATION_HORIZONTAL, 4};
    Gtk::ProgressBar m_scan_progress_bar;
    Gtk::Label       m_scan_progress_label;
    Gtk::Button      m_scan_details_button{"📊 Details"};
    
    // Thumbnail download progress
    Gtk::Box m_download_progress_box{Gtk::ORIENTATION_HORIZONTAL, 5};
    Gtk::Label m_download_status_label;
    Gtk::ProgressBar m_download_progress_bar;
    Gtk::Button m_download_cancel_button{"Cancel"};

    // === Games List ===
    Gtk::ScrolledWindow m_scrolled_games;
    Gtk::TreeView m_treeview_games;
    Glib::RefPtr<Gtk::ListStore> m_model_games;
    ModelColumns m_columns;

    // === Games views: modern list <-> cover grid (Gtk::Stack) ===
    // The dense TreeView (m_scrolled_games) stays in the stack, hidden, as the data
    // + selection backbone; the visible views are the styled ListBox and FlowBox.
    Gtk::Stack          m_view_stack;
    Gtk::ScrolledWindow m_scrolled_grid;
    Gtk::FlowBox        m_flowbox;
    Gtk::ScrolledWindow m_scrolled_mlist;
    Gtk::ListBox        m_mlist;
    Gtk::ToggleButton   m_btn_view_grid;
    Gtk::ToggleButton   m_btn_view_list;
    std::vector<Gtk::TreeRowReference> m_grid_refs;  // card index -> model row
    std::vector<Gtk::TreeRowReference> m_mlist_refs; // list-row index -> model row
    int  m_grid_cap = 600;                           // max items built (perf guard)
    bool m_suppress_view_toggle = false;
    void set_view_mode(bool grid);
    void refresh_active_view();                      // rebuild whichever custom view is shown
    void rebuild_grid();
    void rebuild_mlist();
    Gtk::Widget* make_game_card(const Gtk::TreeModel::Row& row);
    Gtk::Widget* make_list_row(const Gtk::TreeModel::Row& row);
    void on_grid_selection_changed();
    void on_grid_child_activated(Gtk::FlowBoxChild* child);
    void on_mlist_row_selected(Gtk::ListBoxRow* row);
    void on_mlist_row_activated(Gtk::ListBoxRow* row);
    std::string resolve_preview_path(const std::string& name, const std::string& system);

    // === 3-Panel Layout like MAMEUI ===
    Gtk::Paned m_paned_main{Gtk::ORIENTATION_HORIZONTAL}; // Filter panel | Rest
    Gtk::Box m_right_box{Gtk::ORIENTATION_VERTICAL};       // views on top, detail dock at bottom
    Gtk::Box m_details_box{Gtk::ORIENTATION_HORIZONTAL, 14}; // bottom detail dock
    Gtk::Image m_preview_image;
    Gtk::Image m_title_image;
    Gtk::Label m_label_title;
    Gtk::Label m_label_info;
    Gtk::Button m_button_play{"▶ Launch"};
    Gtk::Button m_button_download_art{"🎨 Download Art"};
    Gtk::Box    m_dock_pills{Gtk::ORIENTATION_HORIZONTAL, 6}; // status / zip / CRC pills
    Gtk::Button m_button_favorite{"★"};
    void on_dock_favorite_clicked();
    
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

    // Non-modal scan dialog (heap-allocated, kept alive until user closes it)
    std::unique_ptr<ROMScanDialog> m_scan_dialog;
    sigc::connection m_scan_bg_poll_timer; // polls progress when running in background
    
    // Filter performance optimization
    sigc::connection m_search_timeout_connection;
    std::vector<Game> m_filtered_games;
    std::mutex m_filter_mutex;
    
    // Filter cache data
    FilterCache::FilterData m_filter_cache;
    bool m_filter_cache_loaded = false;

    // Controller profiles (loaded on startup, used by ControllerDialog)
    std::map<std::string, ControllerConfig> m_controller_profiles;
    std::string m_active_controller_profile{"Default"};

    // Launch options (persisted in config.json, applied to every game launch)
    bool m_launch_fullscreen{false};
    bool m_launch_integerscale{false};

    // Helper methods
    std::string find_rom_zip_path(const std::string& rom_name);
    bool        verify_zip_integrity(const std::string& zip_path);
    void        load_launch_prefs();
    void        save_launch_prefs();

    // Theme management (dark / light / system)
    Glib::RefPtr<Gtk::CssProvider> m_css_common;
    Glib::RefPtr<Gtk::CssProvider> m_css_dark;
    std::string m_theme_mode{"dark"};
    void apply_theme(const std::string& mode);
};