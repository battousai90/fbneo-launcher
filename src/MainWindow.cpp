// src/MainWindow.cpp
#include "MainWindow.h"
#include <iostream>
#include "DatParser.h"
#include "SettingsPanel.h"
#include "Game.h"
#include "ModelColumns.h"
#include "RomScanner.h"
#include "ScanCache.h"
#include "AppContext.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include "IconManager.h"

MainWindow::MainWindow() {
    std::cout << "[DEBUG] MainWindow constructor started" << std::endl;
    set_title("fbneo-launcher");
    set_default_size(1200, 700);
    set_border_width(8);

    // === Load config ===
    m_settings_panel.load_from_file(AppContext::get_config_path());

    // === Menu ===
    m_menu_file.set_label("File");
    m_menu_file.set_submenu(m_submenu_file);
    m_menu_bar.append(m_menu_file);

    m_menu_item_settings.set_label("Settings...");
    m_menu_item_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_settings_clicked));
    m_submenu_file.append(m_menu_item_settings);

    m_menu_item_quit.set_label("Quit");
    m_menu_item_quit.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_quit));
    m_submenu_file.append(m_menu_item_quit);

    // === Toolbar ===
    m_toolbar.set_spacing(10);
    
    // Load custom play icon
    std::string play_icon_path = AppContext::get_asset_path("icons/play-icon.svg");
    try {
        auto pixbuf = Gdk::Pixbuf::create_from_file(play_icon_path, 24, 24);
        auto image = Gtk::manage(new Gtk::Image(pixbuf));
        m_toolbar_play.set_image(*image);
    } catch (...) {
        // Fallback to system icon if custom icon fails
        m_toolbar_play.set_image_from_icon_name("media-playback-start", Gtk::ICON_SIZE_BUTTON);
    }
    m_toolbar_play.set_always_show_image(true);
    m_toolbar_play.set_sensitive(false); // Disabled until a game is selected
    m_toolbar.pack_start(m_toolbar_play, Gtk::PACK_SHRINK);
    
    // Load custom search icon
    std::string search_icon_path = AppContext::get_asset_path("icons/search-icon.svg");
    try {
        auto pixbuf = Gdk::Pixbuf::create_from_file(search_icon_path, 24, 24);
        auto image = Gtk::manage(new Gtk::Image(pixbuf));
        m_button_scan.set_image(*image);
    } catch (...) {
        // Fallback to system icon if custom icon fails
        m_button_scan.set_image_from_icon_name("system-search-symbolic", Gtk::ICON_SIZE_BUTTON);
    }
    m_button_scan.set_always_show_image(true);
    m_toolbar.pack_start(m_button_scan, Gtk::PACK_SHRINK);

    m_search_entry.set_placeholder_text("Search game...");
    m_toolbar.pack_start(m_search_entry);

    // === TreeView setup ===
    m_model_games = Gtk::ListStore::create(m_columns);
    m_treeview_games.set_model(m_model_games);

    m_treeview_games.append_column(" ", m_columns.m_col_icon);
    m_treeview_games.append_column("Name", m_columns.m_col_name);
    m_treeview_games.append_column("Title", m_columns.m_col_title);
    m_treeview_games.append_column("Year", m_columns.m_col_year);
    m_treeview_games.append_column("Manufacturer", m_columns.m_col_manufacturer);

    m_treeview_games.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_game_selected));
    m_scrolled_games.add(m_treeview_games);
    m_scrolled_games.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    // === Details ===
    m_preview_image.set_size_request(300, 225);
    m_preview_image.set_halign(Gtk::ALIGN_CENTER);
    m_label_title.set_markup("<b>Select a game to play</b>");
    m_label_title.set_margin_top(10);
    m_label_info.set_text("No game selected");
    m_button_play.set_sensitive(false);
    m_button_play.set_halign(Gtk::ALIGN_CENTER);

    m_details_box.pack_start(m_preview_image, Gtk::PACK_SHRINK);
    m_details_box.pack_start(m_label_title);
    m_details_box.pack_start(m_label_info);
    m_details_box.pack_start(m_button_play, Gtk::PACK_SHRINK);
    m_details_box.set_halign(Gtk::ALIGN_CENTER);
    m_details_box.set_valign(Gtk::ALIGN_START);

    // === Layout ===
    m_paned.pack1(m_scrolled_games, true, true);
    m_paned.pack2(m_details_box, false, false);
    m_paned.set_position(800);

    // === Status Bar ===
    m_status_label.set_margin_end(10);
    m_status_label.set_halign(Gtk::ALIGN_END);
    m_status_label.set_size_request(400, -1);  // Largeur fixe pour le texte de scan

    m_stats_box.set_halign(Gtk::ALIGN_START);
    m_status_box.pack_start(m_stats_box, Gtk::PACK_EXPAND_WIDGET);
    m_status_box.pack_start(m_status_label, Gtk::PACK_SHRINK);

    m_status_box.set_margin_start(6);
    m_status_box.set_margin_end(6);
    m_status_box.set_spacing(10);

    // === Packing ===
    m_main_box.pack_start(m_menu_bar, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_toolbar, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_paned, Gtk::PACK_EXPAND_WIDGET);
    m_main_box.pack_start(m_status_box, Gtk::PACK_SHRINK);

    add(m_main_box);

    // === Load Cache ===
    m_model_games->clear();
    m_cached_games.clear();

    const std::string cache_path = AppContext::get_cache_path();
    if (ScanCache::load(m_cached_games, cache_path)) {
        std::cout << "[INFO] Loaded " << m_cached_games.size() << " games from cache: " << cache_path << std::endl;
        for (const auto& game : m_cached_games) {
            auto row = *m_model_games->append();
            row[m_columns.m_col_icon] = IconManager::get_status_icon(game.status);
            row[m_columns.m_col_name] = game.name;
            row[m_columns.m_col_title] = game.description;
            row[m_columns.m_col_year] = game.year;
            row[m_columns.m_col_manufacturer] = game.manufacturer;
        }
        update_status_bar_stats();
    } else {
            m_status_label.set_text("No cache found. Click 'Scan ROMs' to scan.");
            m_status_label.show();
    }

    // === Signals ===
    m_toolbar_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_start_scan_clicked));

    // === Final setup ===
    show_all_children();
}

MainWindow::~MainWindow() {}

void MainWindow::on_game_selected() {
    auto selection = m_treeview_games.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;

    Gtk::TreeModel::Row row = *iter;
    std::string name = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string thumbnails_path = m_settings_panel.get_thumbnails_path();
    std::string image_path = thumbnails_path + "/" + title + ".png";

    try {
        Glib::RefPtr<Gdk::Pixbuf> pixbuf = Gdk::Pixbuf::create_from_file(image_path);
        if (pixbuf) {
            m_preview_image.set(pixbuf->scale_simple(300, 225, Gdk::INTERP_BILINEAR));
        } else {
            m_preview_image.set_from_icon_name("image-missing", Gtk::ICON_SIZE_DIALOG);
        }
    } catch (...) {
        m_preview_image.set_from_icon_name("image-missing", Gtk::ICON_SIZE_DIALOG);
    }

    m_label_title.set_markup("<b>" + title + "</b>");
    m_label_info.set_text("ROM: " + name);
    m_button_play.set_sensitive(true); // Details panel button
    m_toolbar_play.set_sensitive(true); // Toolbar button
}

void MainWindow::on_play_clicked() {
    auto selection = m_treeview_games.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;

    Gtk::TreeModel::Row row = *iter;
    std::string rom_name = Glib::ustring(row[m_columns.m_col_name]).raw();
    
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    std::string roms_path = m_settings_panel.get_roms_path();
    
    if (fbneo_executable.empty()) {
        m_status_label.set_text("Error: FBNeo executable path not set in Settings");
        m_status_label.show();
        return;
    }
    
    if (roms_path.empty()) {
        m_status_label.set_text("Error: ROMs path not set in Settings");
        m_status_label.show();
        return;
    }
    
    // FBNeo needs ROM paths configured in its config file
    // We'll update the FBNeo config to include our ROM path, then launch
    update_fbneo_config(roms_path);
    
    // Launch FBNeo with just the ROM name (it will find it in configured paths)
    std::string command = "\"" + fbneo_executable + "\" \"" + rom_name + "\" &";
    std::cout << "Launching: " << command << std::endl;
    std::system(command.c_str());
}

void MainWindow::update_fbneo_config(const std::string& roms_path) {
    std::string config_file = std::string(getenv("HOME")) + "/.local/share/fbneo/config/fbneo.ini";
    std::string roms_path_with_slash = roms_path;
    
    // Ensure trailing slash
    if (!roms_path_with_slash.empty() && roms_path_with_slash.back() != '/') {
        roms_path_with_slash += "/";
    }
    
    // Read the current config
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cout << "Warning: Could not open FBNeo config file: " << config_file << std::endl;
        return;
    }
    
    std::vector<std::string> lines;
    std::string line;
    bool path_found = false;
    
    while (std::getline(file, line)) {
        // Check if this ROM path is already configured
        if (line.find("szAppRomPaths[0]") != std::string::npos) {
            if (line.find(roms_path_with_slash) != std::string::npos) {
                path_found = true;
            } else {
                // Update the first ROM path with our path
                line = "szAppRomPaths[0] " + roms_path_with_slash;
                path_found = true;
            }
        }
        lines.push_back(line);
    }
    file.close();
    
    if (path_found) {
        // Write back the updated config
        std::ofstream outfile(config_file);
        for (const auto& l : lines) {
            outfile << l << std::endl;
        }
        outfile.close();
        std::cout << "Updated FBNeo config with ROM path: " << roms_path_with_slash << std::endl;
    }
}

void MainWindow::on_start_scan_clicked() {
    std::string dat_path = m_settings_panel.get_dat_path();
    if (dat_path.empty()) {
        m_status_label.set_text("Error: DAT path not defined");
        m_status_label.show();
        return;
    }

    auto games = DatParser::parse(dat_path);
    if (games.empty()) {
        m_status_label.set_text("Error: No games loaded from DAT file");
        m_status_label.show();
        return;
    }

    std::string roms_path = m_settings_panel.get_roms_path();
    
    // Create and show progress dialog
    ScanProgressDialog dialog(*this);
    dialog.show();
    
    // Start the scan
    dialog.start_scan(games, roms_path);
    
    // Update games list if scan completed successfully
    if (!dialog.is_cancelled()) {
        // Clear current games
        m_model_games->clear();
        m_cached_games.clear();
        
        // Add scanned games to the list
        m_cached_games = dialog.get_scanned_games();
        for (const auto& game : m_cached_games) {
            auto row = *m_model_games->append();
            row[m_columns.m_col_icon] = IconManager::get_status_icon(game.status);
            row[m_columns.m_col_name] = game.name;
            row[m_columns.m_col_title] = game.description;
            row[m_columns.m_col_year] = game.year;
            row[m_columns.m_col_manufacturer] = game.manufacturer;
        }
        
        // Save cache and update stats
        ScanCache::save(m_cached_games, AppContext::get_cache_path());
        update_status_bar_stats();
        m_status_label.hide();
    }
}


void MainWindow::on_settings_clicked() {
    auto dialog = Gtk::Dialog("Settings", *this, Gtk::DIALOG_MODAL);
    dialog.set_default_size(600, 300);
    dialog.get_content_area()->pack_start(m_settings_panel);
    dialog.add_button("Save", Gtk::RESPONSE_OK);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);

    m_settings_panel.show();

    if (dialog.run() == Gtk::RESPONSE_OK) {
        m_settings_panel.save_to_file(AppContext::get_config_path());
    }
}

void MainWindow::on_hide() {
    m_settings_panel.save_to_file(AppContext::get_config_path());
    Gtk::Window::on_hide();
}

void MainWindow::on_quit() {
    m_settings_panel.save_to_file(AppContext::get_config_path());
    Gtk::Main::quit();
}

void MainWindow::update_status_bar_stats() {
    int total = 0, available = 0, incorrect = 0, missing = 0, error = 0;

    for (const auto& game : m_cached_games) {
        total++;
        if (game.status == "available") available++;
        else if (game.status == "incorrect") incorrect++;
        else if (game.status == "missing") missing++;
        else error++;
    }

    // Clear previous stats
    auto children = m_stats_box.get_children();
    for (auto& child : children) {
        m_stats_box.remove(*child);
    }

    // Add new stats
    auto add_stat = [&](const std::string& status, int count, const std::string& label) {
        auto icon = IconManager::get_status_icon(status);
        auto image = Gtk::make_managed<Gtk::Image>(icon);
        auto label_widget = Gtk::make_managed<Gtk::Label>(label + ": " + std::to_string(count));
        label_widget->set_margin_start(2);
        label_widget->set_margin_end(8);

        auto stat_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 2);
        stat_box->pack_start(*image, Gtk::PACK_SHRINK);
        stat_box->pack_start(*label_widget, Gtk::PACK_SHRINK);
        m_stats_box.pack_start(*stat_box, Gtk::PACK_SHRINK);
    };

    add_stat("available", available, "OK");
    add_stat("incorrect", incorrect, "Warning");
    add_stat("error", error, "Error");
    add_stat("missing", missing, "Missing");

    auto total_label = Gtk::make_managed<Gtk::Label>("Total: " + std::to_string(total));
    total_label->set_margin_start(12);
    m_stats_box.pack_start(*total_label, Gtk::PACK_SHRINK);

    m_stats_box.show_all();
}

