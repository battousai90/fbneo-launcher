// src/MainWindow.cpp
#include "MainWindow.h"
#include <iostream>
#include "DatParser.h"
#include "SettingsPanel.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include "Game.h"
#include "ModelColumns.h"
#include "RomScanner.h"
#include "ScanCache.h"
#include "AppContext.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <set>
#include <nlohmann/json.hpp>
#include <algorithm>
#include "IconManager.h"

MainWindow::MainWindow(std::function<void(double, const std::string&)> progress_callback) {
    std::cout << "[DEBUG] MainWindow constructor started" << std::endl;
    
    if (progress_callback) progress_callback(0.75, "Setting up interface...");
    set_title("fbneo-launcher");
    set_default_size(1400, 800);  // Larger default size for better column display
    set_border_width(8);

    // === Load config ===
    m_settings_panel.load_from_file(AppContext::get_config_path());

    // === Menu Bar ===
    // File Menu
    m_menu_file.set_label("File");
    m_menu_file.set_submenu(m_submenu_file);
    m_menu_bar.append(m_menu_file);
    
    m_menu_item_settings.set_label("Launcher Settings...");
    m_menu_item_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_settings_clicked));
    m_submenu_file.append(m_menu_item_settings);
    
    m_menu_item_export_game_list.set_label("Export Game List...");
    m_menu_item_export_game_list.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_export_game_list));
    m_submenu_file.append(m_menu_item_export_game_list);
    
    m_submenu_file.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    
    m_menu_item_quit.set_label("Quit");
    m_menu_item_quit.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_quit));
    m_submenu_file.append(m_menu_item_quit);
    
    // Emulator Menu
    m_menu_emulator.set_label("Emulator");
    m_menu_emulator.set_submenu(m_submenu_emulator);
    m_menu_bar.append(m_menu_emulator);
    
    m_menu_item_fbneo_menu.set_label("Open FBNeo Menu");
    m_menu_item_fbneo_menu.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_fbneo_menu));
    m_submenu_emulator.append(m_menu_item_fbneo_menu);
    
    m_submenu_emulator.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    
    m_menu_item_fullscreen_mode.set_label("Launch in Fullscreen");
    m_menu_item_fullscreen_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_fullscreen_mode));
    m_submenu_emulator.append(m_menu_item_fullscreen_mode);
    
    m_menu_item_windowed_mode.set_label("Launch in Window");
    m_menu_item_windowed_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_windowed_mode));
    m_submenu_emulator.append(m_menu_item_windowed_mode);
    
    m_menu_item_original_resolution.set_label("Use Original Resolution");
    m_menu_item_original_resolution.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_original_resolution));
    m_submenu_emulator.append(m_menu_item_original_resolution);
    
    m_submenu_emulator.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    
    m_menu_item_download_latest_fbneo.set_label("Download Latest FBNeo Release");
    m_menu_item_download_latest_fbneo.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_download_latest_fbneo));
    m_submenu_emulator.append(m_menu_item_download_latest_fbneo);
    
    m_menu_item_generate_dat_files.set_label("Generate DAT Files");
    m_menu_item_generate_dat_files.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_generate_dat_files));
    m_submenu_emulator.append(m_menu_item_generate_dat_files);
    
    // Systems Menu
    m_menu_systems.set_label("Systems");
    m_menu_systems.set_submenu(m_submenu_systems);
    m_menu_bar.append(m_menu_systems);
    
    m_menu_item_arcade_mode.set_label("Arcade Systems Only");
    m_menu_item_arcade_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_arcade_mode));
    m_submenu_systems.append(m_menu_item_arcade_mode);
    
    m_menu_item_console_mode.set_label("Console Systems Only");
    m_menu_item_console_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_console_mode));
    m_submenu_systems.append(m_menu_item_console_mode);
    
    m_menu_item_all_systems.set_label("All Systems");
    m_menu_item_all_systems.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_all_systems));
    m_submenu_systems.append(m_menu_item_all_systems);
    
    // ROMs Menu
    m_menu_roms.set_label("ROMs");
    m_menu_roms.set_submenu(m_submenu_roms);
    m_menu_bar.append(m_menu_roms);
    
    m_menu_item_rescan_roms.set_label("Rescan ROMs");
    m_menu_item_rescan_roms.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_rescan_roms));
    m_submenu_roms.append(m_menu_item_rescan_roms);
    
    m_menu_item_show_available_only.set_label("Show Available ROMs Only");
    m_menu_item_show_available_only.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_show_available_only));
    m_submenu_roms.append(m_menu_item_show_available_only);
    
    m_menu_item_show_missing_roms.set_label("Show Missing ROMs");
    m_menu_item_show_missing_roms.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_show_missing_roms));
    m_submenu_roms.append(m_menu_item_show_missing_roms);
    
    // Help Menu
    m_menu_help.set_label("Help");
    m_menu_help.set_submenu(m_submenu_help);
    m_menu_bar.append(m_menu_help);
    
    m_menu_item_about_fbneo.set_label("About FinalBurn Neo");
    m_menu_item_about_fbneo.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_about_fbneo));
    m_submenu_help.append(m_menu_item_about_fbneo);
    
    m_menu_item_about_launcher.set_label("About Launcher");
    m_menu_item_about_launcher.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_about_launcher));
    m_submenu_help.append(m_menu_item_about_launcher);

    // === Toolbar ===
    m_toolbar_container.set_spacing(2);
    m_toolbar_row1.set_spacing(5);
    m_toolbar_row1.set_margin_start(5);
    m_toolbar_row1.set_margin_end(5);
    m_toolbar_row2.set_spacing(5);
    m_toolbar_row2.set_margin_start(5);
    m_toolbar_row2.set_margin_end(5);
    
    // First row: Action buttons and search
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
    m_toolbar_play.set_size_request(80, 32); // Force minimum size
    m_toolbar_row1.pack_start(m_toolbar_play, Gtk::PACK_SHRINK);
    
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
    m_button_scan.set_size_request(100, 32); // Force minimum size
    m_toolbar_row1.pack_start(m_button_scan, Gtk::PACK_SHRINK);

    m_search_entry.set_placeholder_text("Search game...");
    m_search_entry.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::filter_games));
    m_search_entry.set_size_request(200, 32); // Force minimum size
    m_toolbar_row1.pack_start(m_search_entry, Gtk::PACK_EXPAND_WIDGET);
    
    // Second row: Filters
    // System filter combo
    m_system_filter.append("All Systems");
    m_system_filter.set_active(0);
    m_system_filter.set_size_request(120, 32); // Force minimum size
    m_system_filter.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_system_filter_changed));
    m_toolbar_row2.pack_start(m_system_filter, Gtk::PACK_SHRINK);
    
    // Orientation filter combo
    m_orientation_filter.append("All Orientations");
    m_orientation_filter.append("horizontal");
    m_orientation_filter.append("vertical");
    m_orientation_filter.set_active(0);
    m_orientation_filter.set_size_request(120, 32); // Force minimum size
    m_orientation_filter.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_orientation_filter_changed));
    m_toolbar_row2.pack_start(m_orientation_filter, Gtk::PACK_SHRINK);
    
    // Driver status filter combo
    m_driver_status_filter.append("All Driver Status");
    m_driver_status_filter.append("good");
    m_driver_status_filter.append("preliminary");
    m_driver_status_filter.append("nodump");
    m_driver_status_filter.set_active(0);
    m_driver_status_filter.set_size_request(120, 32); // Force minimum size
    m_driver_status_filter.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_driver_status_filter_changed));
    m_toolbar_row2.pack_start(m_driver_status_filter, Gtk::PACK_SHRINK);
    
    // ROM status filter combo
    m_rom_status_filter.append("All ROMs");
    m_rom_status_filter.append("available");
    m_rom_status_filter.append("missing");
    m_rom_status_filter.append("incorrect");
    m_rom_status_filter.set_active(0);
    m_rom_status_filter.set_size_request(120, 32); // Force minimum size
    m_rom_status_filter.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_rom_status_filter_changed));
    m_toolbar_row2.pack_start(m_rom_status_filter, Gtk::PACK_SHRINK);
    
    // Add spacer to push filters to the left
    auto spacer = Gtk::make_managed<Gtk::Label>("");
    m_toolbar_row2.pack_start(*spacer, Gtk::PACK_EXPAND_WIDGET);
    
    // Pack rows into container
    m_toolbar_container.pack_start(m_toolbar_row1, Gtk::PACK_SHRINK);
    m_toolbar_container.pack_start(m_toolbar_row2, Gtk::PACK_SHRINK);

    // === TreeView setup ===
    m_model_games = Gtk::ListStore::create(m_columns);
    m_treeview_games.set_model(m_model_games);

    m_treeview_games.append_column(" ", m_columns.m_col_icon);
    m_treeview_games.append_column("Name", m_columns.m_col_name);
    m_treeview_games.append_column("Title", m_columns.m_col_title);
    m_treeview_games.append_column("Year", m_columns.m_col_year);
    m_treeview_games.append_column("Manufacturer", m_columns.m_col_manufacturer);
    m_treeview_games.append_column("System", m_columns.m_col_system);
    m_treeview_games.append_column("Type", m_columns.m_col_video_type);
    m_treeview_games.append_column("Orientation", m_columns.m_col_orientation);
    m_treeview_games.append_column("Width", m_columns.m_col_width);
    m_treeview_games.append_column("Height", m_columns.m_col_height);
    m_treeview_games.append_column("Aspect", m_columns.m_col_aspect);
    m_treeview_games.append_column("Driver", m_columns.m_col_driver_status);
    m_treeview_games.append_column("Comment", m_columns.m_col_comment);
    m_treeview_games.append_column("Clone", m_columns.m_col_cloneof);
    m_treeview_games.append_column("Source", m_columns.m_col_sourcefile);

    // Configure column properties for better user experience
    configure_columns();

    m_treeview_games.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_game_selected));
    m_scrolled_games.add(m_treeview_games);
    m_scrolled_games.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    // === Details ===
    m_preview_image.set_size_request(300, 225);
    m_preview_image.set_halign(Gtk::ALIGN_CENTER);
    m_label_title.set_markup("<b>Select a game to play</b>");  // This is safe static text
    m_label_title.set_margin_top(10);
    m_label_info.set_text("No game selected");
    m_button_play.set_sensitive(false);
    m_button_play.set_halign(Gtk::ALIGN_CENTER);
    m_button_play.set_size_request(120, 32); // Force minimum size

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
    m_main_box.pack_start(m_toolbar_container, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_paned, Gtk::PACK_EXPAND_WIDGET);
    m_main_box.pack_start(m_status_box, Gtk::PACK_SHRINK);

    add(m_main_box);

    // === Load Cache ===
    if (progress_callback) progress_callback(0.8, "Loading game database...");
    
    m_model_games->clear();
    m_cached_games.clear();

    const std::string cache_path = AppContext::get_cache_path();
    bool cache_loaded = false;
    
    // Try to load cache if it exists
    if (ScanCache::load(m_cached_games, cache_path)) {
        std::cout << "[INFO] Cache loaded with " << m_cached_games.size() << " games" << std::endl;
        cache_loaded = true;
    }
    
    if (!cache_loaded) {
        // Load all games from DAT files with status "missing"
        if (progress_callback) progress_callback(0.85, "Loading DAT files...");
        std::cout << "[INFO] Loading all games from DAT files..." << std::endl;
        std::string dat_path = m_settings_panel.get_dat_path();
        if (!dat_path.empty()) {
            m_cached_games = DatParser::parseAllDats(dat_path);
            std::cout << "[INFO] Loaded " << m_cached_games.size() << " games from DAT files" << std::endl;
            
            // Save initial cache with all games as missing
            ScanCache::save(m_cached_games, cache_path);
        } else {
            m_status_label.set_text("Error: DAT path not configured. Check settings.");
            m_status_label.show();
        }
    }
    
    // Populate system filter and display games
    if (!m_cached_games.empty()) {
        // Populate system filter
        std::set<std::string> systems;
        for (const auto& game : m_cached_games) {
            if (!game.system.empty()) {
                systems.insert(game.system);
            }
        }
        
        m_system_filter.remove_all();
        m_system_filter.append("All Systems");
        for (const auto& system : systems) {
            m_system_filter.append(system);
        }
        m_system_filter.set_active(0);
        
        // Display all games
        filter_games();
        update_status_bar_stats();
    }

    // === Signals ===
    m_toolbar_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_start_scan_clicked));

    // === Final setup ===
    if (progress_callback) progress_callback(0.95, "Finalizing interface...");
    show_all_children();
    
    if (progress_callback) progress_callback(1.0, "Ready!");
    std::cout << "[DEBUG] MainWindow constructor completed" << std::endl;
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

    m_label_title.set_markup("<b>" + escape_markup(title) + "</b>");
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
    std::vector<std::string> roms_paths = m_settings_panel.get_roms_paths();
    
    if (fbneo_executable.empty()) {
        m_status_label.set_text("Error: FBNeo executable path not set in Settings");
        m_status_label.show();
        return;
    }
    
    if (roms_paths.empty()) {
        m_status_label.set_text("Error: No ROM directories configured in Settings");
        m_status_label.show();
        return;
    }
    
    // FBNeo needs ROM paths configured in its config file
    // We'll update the FBNeo config to include all our ROM paths, then launch
    update_fbneo_config(roms_paths);
    
    // Get the selected game's system to determine launch parameters
    std::string game_system = Glib::ustring(row[m_columns.m_col_system]).raw();
    
    // Set the correct system in FBNeo config before launching
    set_fbneo_system(game_system);
    
    // Adjust ROM name for console systems (they use prefixes in FBNeo)
    std::string fbneo_rom_name = rom_name;
    if (game_system == "NES") {
        fbneo_rom_name = "nes_" + rom_name;
    } else if (game_system == "MSX 1") {
        fbneo_rom_name = "msx_" + rom_name;
    } else if (game_system == "FDS" || game_system == "Nintendo FDS") {
        fbneo_rom_name = "fds_" + rom_name;
    } else if (game_system == "Game Gear" || game_system == "Sega GameGear") {
        fbneo_rom_name = "gg_" + rom_name;
    } else if (game_system == "Master System" || game_system == "Sega MasterSystem") {
        fbneo_rom_name = "sms_" + rom_name;
    } else if (game_system == "Megadrive" || game_system == "Sega Megadrive Genesis") {
        fbneo_rom_name = "md_" + rom_name;
    } else if (game_system == "Sega SG-1000" || game_system == "SG-1000") {
        fbneo_rom_name = "sg1k_" + rom_name;
    } else if (game_system == "ColecoVision") {
        fbneo_rom_name = "cv_" + rom_name;
    } else if (game_system == "ZX Spectrum" || game_system == "Sinclar Spectrum") {
        fbneo_rom_name = "spec_" + rom_name;
    } else if (game_system == "NeoGeo Pocket" || game_system == "Neo Geo Pocket") {
        fbneo_rom_name = "ngp_" + rom_name;
    } else if (game_system == "Fairchild Channel F") {
        fbneo_rom_name = "chf_" + rom_name;
    } else if (game_system == "PC-Engine" || game_system == "NEC PC Engine") {
        fbneo_rom_name = "pce_" + rom_name;
    } else if (game_system == "TurboGrafx 16" || game_system == "NEC TurboGraphX 16") {
        fbneo_rom_name = "tg_" + rom_name;
    } else if (game_system == "SNES") {
        fbneo_rom_name = "snes_" + rom_name;
    } else if (game_system == "SuprGrafx" || game_system == "NEC SGX") {
        fbneo_rom_name = "sgx_" + rom_name;
    }
    
    // Launch FBNeo with the adjusted ROM name
    std::string command = "\"" + fbneo_executable + "\" \"" + fbneo_rom_name + "\" &";
    
    std::cout << "Launching " << game_system << " game: " << command << std::endl;
    std::system(command.c_str());
}

void MainWindow::update_fbneo_config(const std::vector<std::string>& roms_paths) {
    std::string config_file = std::string(getenv("HOME")) + "/.local/share/fbneo/config/fbneo.ini";
    
    // Prepare paths with trailing slashes
    std::vector<std::string> normalized_paths;
    for (const auto& path : roms_paths) {
        std::string path_with_slash = path;
        if (!path_with_slash.empty() && path_with_slash.back() != '/') {
            path_with_slash += "/";
        }
        normalized_paths.push_back(path_with_slash);
    }
    
    // Read the current config to check if paths are already correctly set
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cout << "Warning: Could not open FBNeo config file: " << config_file << std::endl;
        return;
    }
    
    std::vector<std::string> lines;
    std::string line;
    std::vector<std::string> current_paths(20); // FBNeo supports up to 20 ROM paths
    bool config_needs_update = false;
    
    // Read current configuration
    while (std::getline(file, line)) {
        // Check for existing ROM path slots
        for (int i = 0; i < 20; ++i) {
            std::string slot_pattern = "szAppRomPaths[" + std::to_string(i) + "]";
            if (line.find(slot_pattern) != std::string::npos) {
                // Extract the path from the line
                size_t space_pos = line.find(' ');
                if (space_pos != std::string::npos && space_pos + 1 < line.length()) {
                    current_paths[i] = line.substr(space_pos + 1);
                }
                break;
            }
        }
        lines.push_back(line);
    }
    file.close();
    
    // Check if any path needs to be updated
    for (size_t i = 0; i < normalized_paths.size() && i < 20; ++i) {
        if (current_paths[i] != normalized_paths[i]) {
            config_needs_update = true;
            break;
        }
    }
    
    // Check if we need to clear paths beyond our count
    for (size_t i = normalized_paths.size(); i < 20; ++i) {
        if (!current_paths[i].empty()) {
            config_needs_update = true;
            break;
        }
    }
    
    if (!config_needs_update) {
        // Configuration is already up to date
        return;
    }
    
    // Update the configuration
    std::set<int> updated_slots;
    for (auto& line : lines) {
        // Update existing ROM path slots
        for (int i = 0; i < static_cast<int>(normalized_paths.size()) && i < 20; ++i) {
            std::string slot_pattern = "szAppRomPaths[" + std::to_string(i) + "]";
            if (line.find(slot_pattern) != std::string::npos) {
                line = slot_pattern + " " + normalized_paths[i];
                updated_slots.insert(i);
                break;
            }
        }
        
        // Clear paths beyond our count
        for (int i = static_cast<int>(normalized_paths.size()); i < 20; ++i) {
            std::string slot_pattern = "szAppRomPaths[" + std::to_string(i) + "]";
            if (line.find(slot_pattern) != std::string::npos) {
                line = slot_pattern + " ";  // Clear the path
                break;
            }
        }
    }
    
    // Add missing ROM path slots that weren't found in the config
    for (int i = 0; i < static_cast<int>(normalized_paths.size()) && i < 20; ++i) {
        if (updated_slots.find(i) == updated_slots.end()) {
            std::string new_line = "szAppRomPaths[" + std::to_string(i) + "] " + normalized_paths[i];
            lines.push_back(new_line);
        }
    }
    
    // Write back the updated config
    std::ofstream outfile(config_file);
    for (const auto& l : lines) {
        outfile << l << std::endl;
    }
    outfile.close();
    
    std::cout << "Updated FBNeo config with " << normalized_paths.size() << " ROM paths" << std::endl;
}

void MainWindow::set_fbneo_system(const std::string& system) {
    std::string config_file = std::string(getenv("HOME")) + "/.local/share/fbneo/config/fbneo.ini";
    
    // System-specific filter values from FBNeo
    int filter_value = 0; // Default
    
    if (system == "IGS PGM") {
        filter_value = 134217728;
    } else if (system == "Fairchild Channel F") {
        filter_value = 553648128;
    } else if (system == "Taito") {
        filter_value = 184549376;
    } else if (system == "Psykyo") {
        filter_value = 218103808;
    } else if (system == "Kaneko") {
        filter_value = 234881024;
    } else if (system == "IREM") {
        filter_value = 285212672;
    } else if (system == "Data East") {
        filter_value = 318767104;
    } else if (system == "Seta") {
        filter_value = 352321536;
    } else if (system == "Technos") {
        filter_value = 369098752;
    } else if (system == "Megadrive" || system == "Sega Megadrive Genesis") {
        filter_value = 201326592;
    } else if (system == "PC-Engine" || system == "NEC PC Engine") {
        filter_value = 385941504;
    } else if (system == "TurboGrafx 16" || system == "NEC TurboGraphX 16") {
        filter_value = 386007040;
    } else if (system == "SuprGrafx" || system == "NEC SGX") {
        filter_value = 386072576;
    } else if (system == "Sega SG-1000" || system == "SG-1000") {
        filter_value = 419430400;
    } else if (system == "ColecoVision") {
        filter_value = 436207616;
    } else if (system == "Master System" || system == "Sega MasterSystem") {
        filter_value = 402653184;
    } else if (system == "Game Gear" || system == "Sega GameGear") {
        filter_value = 301989888;
    } else if (system == "MSX 1") {
        filter_value = 469762048;
    } else if (system == "ZX Spectrum" || system == "Sinclar Spectrum") {
        filter_value = 486539264;
    } else if (system == "NES") {
        filter_value = 503316480;
    } else if (system == "FDS" || system == "Nintendo FDS") {
        filter_value = 520093696;
    } else if (system == "Capcom CPS 1 2 3") {
        filter_value = 16777216;
    } else if (system == "Cave") {
        filter_value = 100663296;
    } else if (system == "Pre 1990") {
        filter_value = 0;
    } else if (system == "Post 1990") {
        filter_value = 167772160;
    } else if (system == "Midway") {
        filter_value = 452984832;
    } else if (system == "SEGA") {
        filter_value = 33554432;
    } else if (system == "Konami") {
        filter_value = 50331648;
    } else if (system == "Toaplan") {
        filter_value = 67108864;
    } else if (system == "Neogeo") {
        filter_value = 83951616;
    } else if (system == "NeoGeo Pocket") {
        filter_value = 536870912;
    } else {
        filter_value = 2147418112; // Everything as fallback
    }
    
    // Read current config
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cout << "Warning: Could not open FBNeo config file: " << config_file << std::endl;
        return;
    }
    
    std::vector<std::string> lines;
    std::string line;
    bool updated = false;
    
    while (std::getline(file, line)) {
        if (line.find("nFilterSelect ") != std::string::npos) {
            line = "nFilterSelect " + std::to_string(filter_value);
            updated = true;
        }
        lines.push_back(line);
    }
    file.close();
    
    if (updated) {
        // Write back the updated config
        std::ofstream outfile(config_file);
        for (const auto& l : lines) {
            outfile << l << std::endl;
        }
        outfile.close();
        std::cout << "Set FBNeo system to: " << system << " (filter: " << filter_value << ")" << std::endl;
    }
}

void MainWindow::on_start_scan_clicked() {
    std::cout << "[INFO] Starting fresh scan from DATs" << std::endl;
    
    // Get ROM paths from settings panel
    std::vector<std::string> roms_paths = m_settings_panel.get_roms_paths();
    std::cout << "[DEBUG] Scanning " << roms_paths.size() << " ROM paths:" << std::endl;
    for (const auto& path : roms_paths) {
        std::cout << "[DEBUG]   - " << path << std::endl;
    }
    
    if (roms_paths.empty()) {
        m_status_label.set_text("Error: No ROM directories defined");
        m_status_label.show();
        return;
    }
    
    // Load all games fresh from DAT files
    std::string dat_path = m_settings_panel.get_dat_path();
    if (dat_path.empty()) {
        m_status_label.set_text("Error: No DAT path defined");
        m_status_label.show();
        return;
    }
    
    std::cout << "[INFO] Loading all games from DAT files: " << dat_path << std::endl;
    auto fresh_games = DatParser::parseAllDats(dat_path);
    std::cout << "[INFO] Loaded " << fresh_games.size() << " games from DAT files" << std::endl;
    
    // Create and show progress dialog
    ScanProgressDialog dialog(*this);
    dialog.show();
    
    // Start the scan with fresh games from DATs
    dialog.start_scan(fresh_games, roms_paths);
    
    // Update games list if scan completed successfully
    if (!dialog.is_cancelled()) {
        // Update cached games with scan results
        m_cached_games = dialog.get_scanned_games();
        
        // Refresh the display with updated statuses
        filter_games();
        
        // Save updated cache and update stats
        ScanCache::save(m_cached_games, AppContext::get_cache_path());
        update_status_bar_stats();
        m_status_label.hide();
        
        std::cout << "[INFO] Scan completed successfully" << std::endl;
    }
}


void MainWindow::on_settings_clicked() {
    auto dialog = Gtk::Dialog("Settings", *this, Gtk::DIALOG_MODAL);
    dialog.set_default_size(800, 500);
    dialog.get_content_area()->pack_start(m_settings_panel);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("OK", Gtk::RESPONSE_OK);

    m_settings_panel.show();

    if (dialog.run() == Gtk::RESPONSE_OK) {
        m_settings_panel.save_to_file(AppContext::get_config_path());
        on_start_scan_clicked();
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

    // Get current filter values
    std::string selected_system = m_system_filter.get_active_text();
    std::string selected_orientation = m_orientation_filter.get_active_text();
    std::string selected_driver_status = m_driver_status_filter.get_active_text();
    std::string search_text = m_search_entry.get_text();
    
    // Convert search text to lowercase for case-insensitive search
    std::transform(search_text.begin(), search_text.end(), search_text.begin(), ::tolower);

    // Count only games that match current filters
    for (const auto& game : m_cached_games) {
        // Apply same filtering logic as filter_games()
        bool system_matches = (selected_system == "All Systems" || game.system == selected_system);
        bool orientation_matches = (selected_orientation == "All Orientations" || game.orientation == selected_orientation);
        bool driver_matches = (selected_driver_status == "All Driver Status" || game.driver_status == selected_driver_status);
        
        bool search_matches = true;
        if (!search_text.empty()) {
            std::string game_name = game.name;
            std::string game_desc = game.description;
            std::transform(game_name.begin(), game_name.end(), game_name.begin(), ::tolower);
            std::transform(game_desc.begin(), game_desc.end(), game_desc.begin(), ::tolower);
            
            search_matches = (game_name.find(search_text) != std::string::npos || 
                            game_desc.find(search_text) != std::string::npos);
        }
        
        // Only count games that match all filters
        if (system_matches && orientation_matches && driver_matches && search_matches) {
            total++;
            if (game.status == "available") available++;
            else if (game.status == "incorrect") incorrect++;
            else if (game.status == "missing") missing++;
            else error++;
        }
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

void MainWindow::on_system_filter_changed() {
    filter_games();
}

void MainWindow::on_orientation_filter_changed() {
    filter_games();
}

void MainWindow::on_driver_status_filter_changed() {
    filter_games();
}

void MainWindow::on_rom_status_filter_changed() {
    filter_games();
}

void MainWindow::filter_games() {
    m_model_games->clear();
    
    std::string selected_system = m_system_filter.get_active_text();
    std::string selected_orientation = m_orientation_filter.get_active_text();
    std::string selected_driver_status = m_driver_status_filter.get_active_text();
    std::string selected_rom_status = m_rom_status_filter.get_active_text();
    std::string search_text = m_search_entry.get_text();
    
    // Convert search text to lowercase for case-insensitive search
    std::transform(search_text.begin(), search_text.end(), search_text.begin(), ::tolower);
    
    for (const auto& game : m_cached_games) {
        // Check system filter
        bool system_matches = (selected_system == "All Systems" || game.system == selected_system);
        
        // Check orientation filter
        bool orientation_matches = (selected_orientation == "All Orientations" || game.orientation == selected_orientation);
        
        // Check driver status filter
        bool driver_matches = (selected_driver_status == "All Driver Status" || game.driver_status == selected_driver_status);
        
        // Check ROM status filter
        bool rom_status_matches = (selected_rom_status == "All ROMs" || game.status == selected_rom_status);
        
        // Check search filter (case-insensitive)
        bool search_matches = true;
        if (!search_text.empty()) {
            std::string game_name = game.name;
            std::string game_desc = game.description;
            std::transform(game_name.begin(), game_name.end(), game_name.begin(), ::tolower);
            std::transform(game_desc.begin(), game_desc.end(), game_desc.begin(), ::tolower);
            
            search_matches = (game_name.find(search_text) != std::string::npos || 
                            game_desc.find(search_text) != std::string::npos);
        }
        
        if (system_matches && orientation_matches && driver_matches && rom_status_matches && search_matches) {
            auto row = *m_model_games->append();
            row[m_columns.m_col_icon] = IconManager::get_status_icon(game.status);
            row[m_columns.m_col_name] = game.name;
            row[m_columns.m_col_title] = game.description;
            row[m_columns.m_col_year] = game.year;
            row[m_columns.m_col_manufacturer] = game.manufacturer;
            row[m_columns.m_col_system] = game.system;
            row[m_columns.m_col_video_type] = game.video_type;
            row[m_columns.m_col_orientation] = game.orientation;
            row[m_columns.m_col_width] = game.width;
            row[m_columns.m_col_height] = game.height;
            row[m_columns.m_col_aspect] = game.aspect_x + ":" + game.aspect_y;
            row[m_columns.m_col_driver_status] = game.driver_status;
            row[m_columns.m_col_comment] = game.comment;
            row[m_columns.m_col_cloneof] = game.cloneof;
            row[m_columns.m_col_sourcefile] = game.sourcefile;
        }
    }
    
    update_status_bar_stats();
}

void MainWindow::configure_columns() {
    // Get all columns to configure them
    auto columns = m_treeview_games.get_columns();
    
    // Configure each column with proper sorting and sizing
    for (size_t i = 0; i < columns.size(); ++i) {
        auto column = columns[i];
        
        // Disable reordering to prevent GTK layout issues
        column->set_reorderable(false);
        
        // Enable resizing
        column->set_resizable(true);
        
        // Enable sorting (clickable headers)
        column->set_clickable(true);
        
        // Set the sort column ID to match the model column
        column->set_sort_column(static_cast<int>(i));
        
        // Set minimum width and default sizing for better readability
        if (column->get_title() == " ") {
            // Icon column - fixed smaller width, no sorting
            column->set_min_width(40);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(40);
            column->set_resizable(false);
            column->set_clickable(false);  // Icons shouldn't be sortable
        } else if (column->get_title() == "Name") {
            column->set_min_width(80);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(100);
        } else if (column->get_title() == "Title") {
            column->set_min_width(150);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_GROW_ONLY);
            column->set_expand(true);
        } else if (column->get_title() == "System") {
            column->set_min_width(60);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(80);
        } else if (column->get_title() == "Year") {
            column->set_min_width(45);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(50);
        } else if (column->get_title() == "Manufacturer") {
            column->set_min_width(80);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(100);
        } else if (column->get_title() == "Orientation") {
            column->set_min_width(60);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(70);
        } else if (column->get_title() == "Width" || column->get_title() == "Height") {
            column->set_min_width(40);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(50);
        } else if (column->get_title() == "Driver") {
            column->set_min_width(50);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(60);
        } else if (column->get_title() == "Comment") {
            column->set_min_width(80);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(100);
        } else if (column->get_title() == "Source") {
            column->set_min_width(100);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(120);
        } else {
            // Other columns (Type, Aspect, Clone)
            column->set_min_width(50);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(60);
        }
        
        // Enable sorting indicators only for sortable columns
        if (column->get_clickable()) {
            column->set_sort_indicator(true);
        }
    }
    
    // Set TreeView properties for better interaction
    m_treeview_games.set_reorderable(false);  // Disable row reordering (causes issues)
    m_treeview_games.set_headers_clickable(true);  // Make headers clickable for sorting
    m_treeview_games.set_headers_visible(true);   // Ensure headers are visible
    m_treeview_games.set_enable_search(true);     // Enable Ctrl+F search
    m_treeview_games.set_search_column(m_columns.m_col_name);  // Search by name by default
    
    // Enable grid lines for better readability
    m_treeview_games.set_grid_lines(Gtk::TREE_VIEW_GRID_LINES_BOTH);
}

std::string MainWindow::escape_markup(const std::string& text) {
    std::string escaped = text;
    
    // Replace HTML entities to prevent markup parsing errors
    size_t pos = 0;
    while ((pos = escaped.find("&", pos)) != std::string::npos) {
        escaped.replace(pos, 1, "&amp;");
        pos += 5; // Length of "&amp;"
    }
    
    pos = 0;
    while ((pos = escaped.find("<", pos)) != std::string::npos) {
        escaped.replace(pos, 1, "&lt;");
        pos += 4; // Length of "&lt;"
    }
    
    pos = 0;
    while ((pos = escaped.find(">", pos)) != std::string::npos) {
        escaped.replace(pos, 1, "&gt;");
        pos += 4; // Length of "&gt;"
    }
    
    return escaped;
}

// === Menu Handlers ===

void MainWindow::on_export_game_list() {
    // Export game list to CSV/JSON
    Gtk::FileChooserDialog dialog(*this, "Export Game List", Gtk::FILE_CHOOSER_ACTION_SAVE);
    dialog.set_transient_for(*this);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Save", Gtk::RESPONSE_OK);
    
    auto filter_csv = Gtk::FileFilter::create();
    filter_csv->set_name("CSV files");
    filter_csv->add_pattern("*.csv");
    dialog.add_filter(filter_csv);
    
    auto filter_json = Gtk::FileFilter::create();
    filter_json->set_name("JSON files");
    filter_json->add_pattern("*.json");
    dialog.add_filter(filter_json);
    
    if (dialog.run() == Gtk::RESPONSE_OK) {
        std::string filename = dialog.get_filename();
        // TODO: Implement actual export functionality
        std::cout << "Exporting game list to: " << filename << std::endl;
    }
}

void MainWindow::on_fbneo_menu() {
    // Launch FBNeo with menu
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    std::string command = "\"" + fbneo_executable + "\" -menu &";
    std::cout << "Opening FBNeo menu: " << command << std::endl;
    std::system(command.c_str());
}

void MainWindow::on_video_settings() {
    // Open video settings dialog
    Gtk::MessageDialog dialog(*this, "Video Settings", false, Gtk::MESSAGE_INFO);
    dialog.set_secondary_text("Video settings are configured within FBNeo.\nUse 'Emulator > Open FBNeo Menu' to access them.");
    dialog.run();
}

void MainWindow::on_audio_settings() {
    // Open audio settings dialog
    Gtk::MessageDialog dialog(*this, "Audio Settings", false, Gtk::MESSAGE_INFO);
    dialog.set_secondary_text("Audio settings are configured within FBNeo.\nUse 'Emulator > Open FBNeo Menu' to access them.");
    dialog.run();
}

void MainWindow::on_input_settings() {
    // Open input settings dialog
    Gtk::MessageDialog dialog(*this, "Input Settings", false, Gtk::MESSAGE_INFO);
    dialog.set_secondary_text("Input settings are configured within FBNeo.\nUse 'Emulator > Open FBNeo Menu' to access them or press F5 during gameplay.");
    dialog.run();
}

void MainWindow::on_fullscreen_mode() {
    // Set fullscreen launch mode
    std::cout << "Setting launch mode to fullscreen" << std::endl;
    // TODO: Store preference for fullscreen mode
}

void MainWindow::on_windowed_mode() {
    // Set windowed launch mode
    std::cout << "Setting launch mode to windowed" << std::endl;
    // TODO: Store preference for windowed mode (-w flag)
}

void MainWindow::on_original_resolution() {
    // Set original resolution mode
    std::cout << "Setting launch mode to original resolution" << std::endl;
    // TODO: Store preference for original resolution (-a flag)
}

void MainWindow::on_arcade_mode() {
    // Filter to show only arcade systems
    std::cout << "Filtering to arcade systems only" << std::endl;
    // TODO: Implement arcade-only filter
}

void MainWindow::on_console_mode() {
    // Filter to show only console systems
    std::cout << "Filtering to console systems only" << std::endl;
    // TODO: Implement console-only filter
}

void MainWindow::on_all_systems() {
    // Show all systems
    std::cout << "Showing all systems" << std::endl;
    m_system_filter.set_active(0); // "All Systems"
}

void MainWindow::on_rescan_roms() {
    // Trigger ROM rescan
    on_start_scan_clicked();
}

void MainWindow::on_verify_roms() {
    // Verify ROM integrity
    Gtk::MessageDialog dialog(*this, "ROM Verification", false, Gtk::MESSAGE_INFO);
    dialog.set_secondary_text("ROM verification will be implemented in a future version.\nCurrently, the scan process validates ROM CRC checksums.");
    dialog.run();
}

void MainWindow::on_show_available_only() {
    // Filter to show only available ROMs
    m_rom_status_filter.set_active_text("available");
}

void MainWindow::on_show_missing_roms() {
    // Filter to show missing ROMs
    m_rom_status_filter.set_active_text("missing");
}

void MainWindow::on_rom_info() {
    // Show ROM information for selected game
    auto selection = m_treeview_games.get_selection();
    if (selection) {
        auto row = selection->get_selected();
        if (row) {
            Glib::ustring name = row->get_value(m_columns.m_col_name);
            Glib::ustring system = row->get_value(m_columns.m_col_system);
            Glib::ustring status = row->get_value(m_columns.m_col_status);
            
            std::string info = "Game: " + name.raw() + "\n";
            info += "System: " + system.raw() + "\n";
            info += "Status: " + status.raw();
            
            Gtk::MessageDialog dialog(*this, "ROM Information", false, Gtk::MESSAGE_INFO);
            dialog.set_secondary_text(info);
            dialog.run();
        }
    }
}

void MainWindow::on_about_fbneo() {
    // Create a custom dialog with buttons for documentation
    Gtk::Dialog dialog("About FinalBurn Neo", *this, true);
    dialog.set_default_size(500, 300);
    
    // Get FBNeo directory path
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    std::filesystem::path fbneo_path(fbneo_executable);
    std::string fbneo_dir = fbneo_path.parent_path().string();
    
    // Create content
    auto content_area = dialog.get_content_area();
    
    auto label = Gtk::manage(new Gtk::Label());
    std::string info_text = "<b>FinalBurn Neo</b>\n\n";
    info_text += "A powerful arcade and console emulator\n";
    info_text += "based on FinalBurn Alpha\n\n";
    info_text += "<b>Executable:</b> " + fbneo_executable + "\n";
    info_text += "<b>Version:</b> v1.0.0.03";
    
    label->set_markup(info_text);
    label->set_line_wrap(true);
    label->set_justify(Gtk::JUSTIFY_CENTER);
    content_area->pack_start(*label);
    
    // Add buttons for documentation
    auto button_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 5));
    button_box->set_margin_top(20);
    
    // GitHub link button
    auto github_button = Gtk::manage(new Gtk::Button("🌐 Official GitHub Repository"));
    github_button->signal_clicked().connect([this]() {
        std::system("xdg-open https://github.com/finalburnneo/FBNeo &");
    });
    button_box->pack_start(*github_button);
    
    // License button
    auto license_button = Gtk::manage(new Gtk::Button("📜 View License"));
    license_button->signal_clicked().connect([fbneo_dir]() {
        std::string license_path = fbneo_dir + "/license.txt";
        if (std::filesystem::exists(license_path)) {
            std::string command = "xdg-open \"" + license_path + "\" &";
            std::system(command.c_str());
        }
    });
    button_box->pack_start(*license_button);
    
    // What's New button
    auto whatsnew_button = Gtk::manage(new Gtk::Button("🆕 What's New"));
    whatsnew_button->signal_clicked().connect([fbneo_dir]() {
        std::string whatsnew_path = fbneo_dir + "/whatsnew.html";
        if (std::filesystem::exists(whatsnew_path)) {
            std::string command = "xdg-open \"" + whatsnew_path + "\" &";
            std::system(command.c_str());
        }
    });
    button_box->pack_start(*whatsnew_button);
    
    // Help file button
    auto help_button = Gtk::manage(new Gtk::Button("❓ Help Documentation"));
    help_button->signal_clicked().connect([fbneo_dir]() {
        std::string help_path = fbneo_dir + "/fbneo.chm";
        if (std::filesystem::exists(help_path)) {
            std::string command = "xdg-open \"" + help_path + "\" &";
            std::system(command.c_str());
        }
    });
    button_box->pack_start(*help_button);
    
    content_area->pack_start(*button_box);
    
    // Close button
    dialog.add_button("Close", Gtk::RESPONSE_CLOSE);
    
    dialog.show_all();
    dialog.run();
}

void MainWindow::on_controls_help() {
    // Show game controls help
    std::string help_text = "Common Game Controls:\n\n";
    help_text += "Arrow Keys: Movement\n";
    help_text += "Z, X, C, V: Action buttons\n";
    help_text += "1, 2: Start Player 1/2\n";
    help_text += "5, 6: Insert Coin\n";
    help_text += "F3: Reset Game\n";
    help_text += "F5: Configure Controls\n";
    help_text += "ESC: Exit Game\n\n";
    help_text += "For system-specific controls, refer to the game's documentation.";
    
    Gtk::MessageDialog dialog(*this, "Game Controls", false, Gtk::MESSAGE_INFO);
    dialog.set_secondary_text(help_text);
    dialog.run();
}

void MainWindow::on_about_launcher() {
    // Show launcher information
    std::string about_text = "FBNeo Launcher\n\n";
    about_text += "A modern launcher for FinalBurn Neo emulator\n";
    about_text += "Supporting multiple arcade and console systems\n\n";
    about_text += "Features:\n";
    about_text += "• Multi-system ROM management\n";
    about_text += "• Advanced filtering and search\n";
    about_text += "• ROM status tracking\n";
    about_text += "• Game thumbnails and details\n";
    about_text += "• FBNeo integration";
    
    Gtk::MessageDialog dialog(*this, "About FBNeo Launcher", false, Gtk::MESSAGE_INFO);
    dialog.set_secondary_text(about_text);
    dialog.run();
}

void MainWindow::on_download_latest_fbneo() {
    auto download_dialog = std::make_unique<DownloadDialog>(
        *this,
        "https://github.com/finalburnneo/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        std::filesystem::current_path().string()
    );
    
    download_dialog->set_settings_entry(&m_settings_panel.m_entry_fbneo);
    download_dialog->start_download();
    download_dialog->run();
}

void MainWindow::on_generate_dat_files() {
    GenerateDAT::execute(*this, m_settings_panel.get_fbneo_executable());
}

