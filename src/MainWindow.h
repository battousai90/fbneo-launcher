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
    void save_scan_cache(const std::string& filename);
    bool load_scan_cache(const std::string& filename);
    void update_status_bar_stats();
    void on_start_scan_clicked();
    void update_fbneo_config(const std::string& roms_path);

    // === Widgets ===
    SettingsPanel m_settings_panel;

    // === Menu Bar ===
    Gtk::MenuBar m_menu_bar;
    Gtk::MenuItem m_menu_file;
    Gtk::Menu m_submenu_file;
    Gtk::MenuItem m_menu_item_settings;
    Gtk::MenuItem m_menu_item_quit;

    // === Toolbar ===
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_toolbar{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button m_toolbar_play{"▶ Play"}; // Toolbar button to play selected game
    Gtk::Button m_button_scan{"Scan ROMs"}; // Button to scan for ROMs
    std::vector<Game> m_cached_games; // Cache for games
    Gtk::Entry m_search_entry; // Search entry for filtering games

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