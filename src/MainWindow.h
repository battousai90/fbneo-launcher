// src/MainWindow.h
#pragma once

#include <gtkmm.h>
#include <string>
#include <vector>
#include "SettingsPanel.h"

struct Game {
    std::string name;
    std::string title;
    std::string year;
    std::string manufacturer;
};

class MainWindow : public Gtk::Window {
public:
    MainWindow();
    virtual ~MainWindow();

private:
    // === Widgets ===
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL};         // Main Box
    Gtk::Box m_toolbar{Gtk::ORIENTATION_HORIZONTAL};        // Toolbar
    Gtk::Button m_button_refresh{"Refresh"};                // Refresh button
    Gtk::Entry m_search_entry;
    Gtk::Paned m_paned{Gtk::ORIENTATION_HORIZONTAL};        // Paned container for game list and details
    Gtk::ScrolledWindow m_scrolled_games;                   // Scrolled window for game list
    Gtk::ListBox m_listbox_games;                           // ListBox for games
    Gtk::Box m_details_box{Gtk::ORIENTATION_VERTICAL};      // Box for game details
    Gtk::Image m_preview_image;
    Gtk::Label m_label_title;
    Gtk::Label m_label_info;
    Gtk::Button m_button_play{"▶ Play"};
    SettingsPanel m_settings_panel; // Settings panel
    Gtk::Notebook m_notebook; // Notebook for switching between game list and settings

    // === Méthodes ===
    void populate_game_list();
    void on_game_selected();
    void on_play_clicked();
    void on_refresh_clicked();
    protected:
    void on_hide() override;
};