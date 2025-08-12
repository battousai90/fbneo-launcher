// src/MainWindow.cpp
#include "MainWindow.h"
#include <iostream>
#include "GameRow.h"
#include "DatParser.h"
#include "SettingsPanel.h"
#include "Game.h"
#include "ModelColumns.h"
#include "RomScanner.h"
#include "ScanCache.h"

MainWindow::MainWindow() {
    std::cout << "[DEBUG] MainWindow constructor started" << std::endl;

    set_title("fbneo-launcher");
    set_default_size(1200, 700);
    set_border_width(8);

    // === Initialize Widgets ===
    std::cout << "[DEBUG] Loading config..." << std::endl;
    m_settings_panel.load_from_file("config.json");

    // === Build UI Components ===
    std::cout << "[DEBUG] Building menu..." << std::endl;
    m_menu_file.set_label("File");
    m_menu_file.set_submenu(m_submenu_file);
    m_menu_bar.append(m_menu_file);

    // Menu items
    m_menu_item_settings.set_label("Settings...");
    m_menu_item_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_settings_clicked));
    m_submenu_file.append(m_menu_item_settings);

    // Quit item
    m_menu_item_quit.set_label("Quit");
    m_menu_item_quit.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_hide));
    m_submenu_file.append(m_menu_item_quit);

    // Add menu bar to the main box
    std::cout << "[DEBUG] Building toolbar..." << std::endl;
    m_toolbar.set_spacing(10);
    // Add a button to scan for ROMs
    m_button_scan.set_image_from_icon_name("system-search-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_scan.set_always_show_image(true);
    m_toolbar.pack_start(m_button_scan, Gtk::PACK_SHRINK);
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_scan_clicked));
    // Add a search entry
    m_search_entry.set_placeholder_text("Search game...");
    m_toolbar.pack_start(m_search_entry);

    // === Setup Games List ===
    std::cout << "[DEBUG] Setting up TreeView..." << std::endl;
    m_model_games = Gtk::ListStore::create(m_columns);
    m_treeview_games.set_model(m_model_games);
    // Set up the columns
    m_treeview_games.append_column(" ", m_columns.m_col_icon);
    m_treeview_games.append_column("Name", m_columns.m_col_name);
    m_treeview_games.append_column("Title", m_columns.m_col_title);
    m_treeview_games.append_column("Year", m_columns.m_col_year);
    m_treeview_games.append_column("Manufacturer", m_columns.m_col_manufacturer);
    // Set up the TreeView
    m_treeview_games.get_selection()->signal_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_game_selected)
    );
    // Set up the scrolled window
    m_scrolled_games.add(m_treeview_games);
    m_scrolled_games.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    // === Setup Game Details ===
    std::cout << "[DEBUG] Setting up details box..." << std::endl;
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

    std::cout << "[DEBUG] Setting up main layout..." << std::endl;
    m_paned.pack1(m_scrolled_games, true, true);
    m_paned.pack2(m_details_box, false, false);
    m_paned.set_position(800);

    std::cout << "[DEBUG] Packing main box..." << std::endl;
    m_main_box.pack_start(m_menu_bar, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_toolbar, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_paned);

        // === Status Bar ===
    std::cout << "[DEBUG] Building status bar..." << std::endl;
    m_status_context = m_statusbar.get_context_id("status");
    m_main_box.pack_start(m_statusbar, Gtk::PACK_SHRINK);
    m_statusbar.set_margin_start(6);
    m_statusbar.set_margin_end(6);
    m_statusbar.set_margin_bottom(0);

    add(m_main_box);

    // === Loading Cache ===
    m_model_games->clear();
    m_cached_games.clear();

    if (ScanCache::load(m_cached_games, "cache/scan_cache.json")) {
        std::cout << "[INFO] Loaded " << m_cached_games.size() << " games from cache" << std::endl;
        for (const auto& game : m_cached_games) {
            auto row = *m_model_games->append();
            row[m_columns.m_col_icon] = get_status_icon(game.status);
            row[m_columns.m_col_name] = game.name;
            row[m_columns.m_col_title] = game.description;
            row[m_columns.m_col_year] = game.year;
            row[m_columns.m_col_manufacturer] = game.manufacturer;
        }
        update_status_bar_stats(); // Update status bar with game count
    } else {
        m_statusbar.push("No cache found. Click 'Scan ROMs' to scan.", m_status_context);
    }

    std::cout << "[DEBUG] Connecting signals..." << std::endl;
    m_button_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));

    std::cout << "[DEBUG] Calling show_all_children()..." << std::endl;
    show_all_children();

    std::cout << "[DEBUG] MainWindow constructor finished" << std::endl;
}

MainWindow::~MainWindow() {}

Glib::RefPtr<Gdk::Pixbuf> MainWindow::get_status_icon(const std::string& status) {
    std::string icon_name;
    if (status == "available") {
        icon_name = "emblem-ok-symbolic";        // Green
    } else if (status == "incorrect") {
        icon_name = "dialog-warning-symbolic";   // yellow
    } else if (status == "missing") {
        icon_name = "image-missing-symbolic";    // grey
    } else {
        icon_name = "dialog-error-symbolic";     // Red
    }
    return Gtk::IconTheme::get_default()->load_icon(icon_name, Gtk::ICON_SIZE_SMALL_TOOLBAR);
}

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
    } catch (const Glib::FileError& e) {
        m_preview_image.set_from_icon_name("image-missing", Gtk::ICON_SIZE_DIALOG);
    } catch (const Gdk::PixbufError& e) {
        m_preview_image.set_from_icon_name("image-missing", Gtk::ICON_SIZE_DIALOG);
    }

    m_label_title.set_markup("<b>" + title + "</b>");
    m_label_info.set_text("ROM: " + name);
    m_button_play.set_sensitive(true);
}

void MainWindow::on_play_clicked() {
    auto selection = m_treeview_games.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;

    Gtk::TreeModel::Row row = *iter;
    std::string rom_name = Glib::ustring(row[m_columns.m_col_name]).raw();

    std::string command = m_settings_panel.get_fbneo_executable() + " \"" + rom_name + "\" &";
    std::system(command.c_str());
}

void MainWindow::on_scan_clicked() {
    std::cout << "[INFO] Starting ROM scan..." << std::endl;

    m_model_games->clear();
    m_cached_games.clear();

    std::string dat_path = m_settings_panel.get_dat_path();
    if (dat_path.empty()) {
        std::cerr << "Error: DAT path not defined" << std::endl;
        m_statusbar.push("Error: DAT path not defined", m_status_context);
        return;
    }

    auto games = DatParser::parse(dat_path);
    if (games.empty()) {
        std::cerr << "No games loaded from: " << dat_path << std::endl;
        m_statusbar.push("Error: No games loaded from DAT file", m_status_context);
        return;
    }

    std::string roms_path = m_settings_panel.get_roms_path();

    int total = games.size();
    int current = 0;

    for (const auto& game : games) {
        Game mutable_game = game;
        RomScanner::check_availability(mutable_game, roms_path);
        m_cached_games.push_back(mutable_game);

        auto row = *m_model_games->append();
        row[m_columns.m_col_icon] = get_status_icon(mutable_game.status);
        row[m_columns.m_col_name] = mutable_game.name;
        row[m_columns.m_col_title] = mutable_game.description;
        row[m_columns.m_col_year] = mutable_game.year;
        row[m_columns.m_col_manufacturer] = mutable_game.manufacturer;

        current++;
        if (current % 10 == 0) {  // Mise à jour toutes les 10 ROMs
            update_status_bar_scanning(current, total);
            while (Gtk::Main::events_pending()) Gtk::Main::iteration();  // Garde l'UI réactive
        }
    }

    // Save the scan cache
    if (ScanCache::save(m_cached_games, "cache/scan_cache.json")) {
        std::cout << "[INFO] Scan saved to cache/scan_cache.json" << std::endl;
    }
    // Update the status bar with the total number of games scanned
    update_status_bar_stats();
}

// Handle the settings dialog
void MainWindow::on_settings_clicked() {
    auto dialog = Gtk::Dialog("Settings", *this, Gtk::DIALOG_MODAL);
    dialog.set_default_size(600, 300);
    dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    dialog.get_content_area()->pack_start(m_settings_panel);
    dialog.add_button("Save", Gtk::RESPONSE_OK);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);

    m_settings_panel.show();

    int result = dialog.run();
    if (result == Gtk::RESPONSE_OK) {
        m_settings_panel.save_to_file("config.json");
    }
}

// Update the status bar with game statistics
void MainWindow::update_status_bar_stats() {
    int total = 0, available = 0, incorrect = 0, missing = 0, error = 0;

    for (const auto& game : m_cached_games) {
        total++;
        if (game.status == "available") {
            available++;
        } else if (game.status == "incorrect") {
            incorrect++;
        } else if (game.status == "missing") {
            missing++;
        } else {
            error++;
        }
    }

    std::ostringstream oss;
    oss << "Total: " << total
        << " | OK: " << available
        << " | Warning: " << incorrect
        << " | Error: " << error
        << " | Missing: " << missing;

    m_statusbar.push(oss.str(), m_status_context);
}

// Update the status bar during scanning
void MainWindow::update_status_bar_scanning(int current, int total) {
    int percent = total > 0 ? (current * 100) / total : 0;
    std::ostringstream oss;
    oss << "Scanning... " << current << "/" << total << " games processed (" << percent << "%)";
    m_statusbar.push(oss.str(), m_status_context);
}

// Handle the hide event
void MainWindow::on_hide() {
    m_settings_panel.save_to_file("config.json");
    Gtk::Window::on_hide();
}