// src/MainWindow.cpp
#include "MainWindow.h"
#include "i18n.h"
#include <iostream>
#include "DatParser.h"
#include "SettingsPanel.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include "Game.h"
#include "ModelColumns.h"
#include "RomScanner.h"
#include "AppContext.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <set>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <thread>
#include "IconManager.h"
#include "ControllerDialog.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <zip.h>
#include <sstream>

// Launch an external process without invoking a shell.
// args[0] must be the executable path; remaining entries are its arguments.
// Returns the child PID on success, -1 on failure.
static pid_t spawn_process(const std::vector<std::string>& args) {
    if (args.empty()) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[ERROR] fork() failed for: " << args[0] << std::endl;
        return -1;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::cerr << "[ERROR] execvp failed for: " << args[0] << std::endl;
        _exit(1);
    }
    return pid; // parent gets child PID
}

// Watch a child process and record its playtime in the database when it exits.
static void watch_playtime(pid_t pid,
                            std::shared_ptr<DatabaseManager> db,
                            const std::string& game_name,
                            const std::string& system)
{
    auto start = std::chrono::steady_clock::now();
    int status = 0;
    waitpid(pid, &status, 0); // blocking wait
    auto end = std::chrono::steady_clock::now();
    int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
    if (elapsed > 0)
        db->addPlayTime(game_name, system, elapsed);
}

MainWindow::MainWindow(std::shared_ptr<DatabaseManager> database,
                       std::function<void(double, const std::string&)> progress_callback,
                       const std::vector<Game>& preloaded_games) {
    std::cout << "[DEBUG] MainWindow constructor started" << std::endl;

    if (progress_callback) progress_callback(0.75, "Setting up interface...");
    set_title("fbneo-launcher");
    set_default_size(1400, 800);  // Larger default size for better column display
    set_border_width(8);

    // === Modern theme ===
    // Providers are created and applied by apply_theme(); the actual mode is set
    // once settings are loaded (see below). Default to dark until then.
    apply_theme("dark");
    // Tag widgets so the stylesheet can target them.
    m_toolbar_play.get_style_context()->add_class("accent-button");
    m_button_play.get_style_context()->add_class("accent-button");
    m_search_entry.get_style_context()->add_class("search-entry");
    m_toolbar_container.get_style_context()->add_class("app-toolbar");
    m_scrolled_filters.get_style_context()->add_class("sidebar");
    m_status_box.get_style_context()->add_class("statusbar");

    // === Database ===
    // Reuse the connection opened in main() — opening a second sqlite3 handle on
    // the same file caused write contention and double-init noise in the log.
    m_database = database;
    if (!m_database) {
        std::cerr << "[ERROR] No database handle passed to MainWindow" << std::endl;
        m_status_label.set_text("Error: Failed to initialize database");
        m_status_label.show();
    }

    // === Load config ===
    m_settings_panel.load_from_file(AppContext::get_config_path());
    load_launch_prefs();

    // Apply the saved theme, and react to theme/language changes from Settings.
    apply_theme(m_settings_panel.get_theme());
    m_settings_panel.signal_theme_changed().connect([this](Glib::ustring mode) {
        apply_theme(mode);
        m_settings_panel.save_to_file(AppContext::get_config_path());
    });
    m_settings_panel.signal_language_changed().connect([this](Glib::ustring code) {
        on_language_selected(code);
    });

    // === Menu Bar ===
    // File Menu
    m_menu_file.set_label("File");
    m_menu_file.set_submenu(m_submenu_file);
    m_app_menu.append(m_menu_file);
    
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
    m_app_menu.append(m_menu_emulator);
    
    m_menu_item_fbneo_menu.set_label("Open FBNeo Menu");
    m_menu_item_fbneo_menu.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_fbneo_menu));
    m_submenu_emulator.append(m_menu_item_fbneo_menu);

    m_menu_item_input_settings.set_label("Controller Settings...");
    m_menu_item_input_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_input_settings));
    m_submenu_emulator.append(m_menu_item_input_settings);

    m_submenu_emulator.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    
    m_menu_item_fullscreen_mode.set_label("Launch in Fullscreen  (-fullscreen)");
    m_menu_item_fullscreen_mode.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::on_fullscreen_mode));
    m_submenu_emulator.append(m_menu_item_fullscreen_mode);

    m_menu_item_integerscale_mode.set_label("Use Integer Scale  (-integerscale)");
    m_menu_item_integerscale_mode.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::on_integerscale_mode));
    m_submenu_emulator.append(m_menu_item_integerscale_mode);
    
    m_submenu_emulator.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    
    m_menu_item_download_latest_fbneo.set_label("Download Latest FBNeo Release");
    m_menu_item_download_latest_fbneo.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_download_latest_fbneo));
    m_submenu_emulator.append(m_menu_item_download_latest_fbneo);
    
    m_menu_item_generate_dat_files.set_label("Generate DAT Files");
    m_menu_item_generate_dat_files.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_generate_dat_files));
    m_submenu_emulator.append(m_menu_item_generate_dat_files);
    
    // Filter Menu
    m_menu_filter.set_label("Filter");
    m_menu_filter.set_submenu(m_submenu_filter);
    m_app_menu.append(m_menu_filter);
    
    m_menu_item_all_systems.set_label("Show All Games");
    m_menu_item_all_systems.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_all_systems));
    m_submenu_filter.append(m_menu_item_all_systems);
    
    m_menu_item_arcade_mode.set_label("Arcade Games Only");
    m_menu_item_arcade_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_arcade_mode));
    m_submenu_filter.append(m_menu_item_arcade_mode);
    
    m_menu_item_console_mode.set_label("Console Games Only");
    m_menu_item_console_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_console_mode));
    m_submenu_filter.append(m_menu_item_console_mode);
    
    m_menu_item_show_available_only.set_label("Available ROMs Only");
    m_menu_item_show_available_only.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_show_available_only));
    m_submenu_filter.append(m_menu_item_show_available_only);
    
    m_menu_item_show_missing_roms.set_label("Missing ROMs Only");
    m_menu_item_show_missing_roms.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_show_missing_roms));
    m_submenu_filter.append(m_menu_item_show_missing_roms);
    
    // ROMs Menu
    m_menu_roms.set_label("ROMs");
    m_menu_roms.set_submenu(m_submenu_roms);
    m_app_menu.append(m_menu_roms);
    
    m_menu_item_rescan_roms.set_label("Rescan ROMs");
    m_menu_item_rescan_roms.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_rescan_roms));
    m_submenu_roms.append(m_menu_item_rescan_roms);
    
    m_menu_item_update_dat.set_label("Update DAT");
    m_menu_item_update_dat.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_update_dat_clicked));
    m_submenu_roms.append(m_menu_item_update_dat);

    m_menu_item_find_duplicates.set_label("Find Duplicate ROMs...");
    m_menu_item_find_duplicates.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_find_duplicate_roms));
    m_submenu_roms.append(m_menu_item_find_duplicates);

    // Help Menu
    m_menu_help.set_label("Help");
    m_menu_help.set_submenu(m_submenu_help);
    m_app_menu.append(m_menu_help);
    
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
    // Scan becomes an icon-only action on the right of the header (see header block).
    std::string search_icon_path = AppContext::get_asset_path("icons/search-icon.svg");
    try {
        auto pixbuf = Gdk::Pixbuf::create_from_file(search_icon_path, 18, 18);
        auto image = Gtk::manage(new Gtk::Image(pixbuf));
        m_button_scan.set_image(*image);
    } catch (...) {
        m_button_scan.set_image_from_icon_name("view-refresh-symbolic", Gtk::ICON_SIZE_BUTTON);
    }
    m_button_scan.set_always_show_image(true);
    m_button_scan.set_tooltip_text(_("Scan ROMs"));

    // View toggle: list <-> cover grid (segmented). Packed on the right below.
    m_btn_view_list.set_label("≡");
    m_btn_view_grid.set_label("▦");
    m_btn_view_list.set_tooltip_text(_("List view"));
    m_btn_view_grid.set_tooltip_text(_("Grid view"));
    m_btn_view_list.set_active(true);
    m_btn_view_list.signal_toggled().connect([this] {
        if (!m_suppress_view_toggle && m_btn_view_list.get_active()) set_view_mode(false);
    });
    m_btn_view_grid.signal_toggled().connect([this] {
        if (!m_suppress_view_toggle && m_btn_view_grid.get_active()) set_view_mode(true);
    });

    m_search_entry.set_placeholder_text(_("Search game..."));
    m_search_entry.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::filter_games_async));
    m_search_entry.set_size_request(360, 32);
    m_headerbar.set_custom_title(m_search_entry); // centered search

    // Labels for the buttons that live in the detail dock.
    m_button_play.set_label(_("▶ Launch"));
    m_button_download_art.set_label(_("🎨 Download Art"));
    
    // Second row: Keep empty for now - filters will be in left panel
    // m_toolbar_row2 kept for future use
    
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
    m_treeview_games.append_column_editable("★", m_columns.m_col_favorite);
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

    // Connect favourite toggle: clicking the ★ checkbox toggles favourite in DB
    {
        auto* fav_col = m_treeview_games.get_column(1); // col index 1 = ★
        if (fav_col) {
            auto* cell = dynamic_cast<Gtk::CellRendererToggle*>(fav_col->get_first_cell());
            if (cell) {
                cell->signal_toggled().connect([this](const Glib::ustring& path_str) {
                    auto iter = m_model_games->get_iter(path_str);
                    if (!iter) return;
                    Gtk::TreeModel::Row row = *iter;
                    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
                    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
                    m_database->toggleFavorite(name, system);
                    bool now_fav = m_database->isFavorite(name, system);
                    row[m_columns.m_col_favorite] = now_fav;
                    // Update cached game too
                    for (auto& g : m_cached_games)
                        if (g.name == name && g.system == system) { g.is_favorite = now_fav; break; }
                });
            }
        }
    }

    m_treeview_games.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_game_selected));
    m_scrolled_games.add(m_treeview_games);
    m_scrolled_games.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    // === Details ===
    m_preview_image.set_size_request(128, 96);
    m_preview_image.set_halign(Gtk::ALIGN_CENTER);
    m_label_title.set_markup("<b>Select a game to play</b>");  // This is safe static text
    m_label_title.set_margin_top(10);
    m_label_info.set_text("No game selected");
    m_button_play.set_sensitive(false);
    m_button_play.set_halign(Gtk::ALIGN_CENTER);
    m_button_play.set_size_request(120, 32); // Force minimum size

    m_button_download_art.set_sensitive(false);
    m_button_download_art.set_halign(Gtk::ALIGN_CENTER);
    m_button_download_art.set_size_request(140, 32); // Slightly wider for "Download Art"

    // Bottom detail dock (horizontal): thumbnail | title + info | actions.
    m_details_box.get_style_context()->add_class("detail-dock");
    m_details_box.set_margin_start(12);
    m_details_box.set_margin_end(12);
    m_details_box.set_margin_top(8);
    m_details_box.set_margin_bottom(8);

    m_preview_image.set_valign(Gtk::ALIGN_CENTER);
    m_preview_image.get_style_context()->add_class("dock-thumb");
    m_details_box.pack_start(m_preview_image, Gtk::PACK_SHRINK);

    auto* info_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    info_box->set_valign(Gtk::ALIGN_CENTER);
    m_label_title.set_xalign(0.0f);
    m_label_title.get_style_context()->add_class("dock-title");
    m_label_info.set_xalign(0.0f);
    m_label_info.set_ellipsize(Pango::ELLIPSIZE_END);
    m_label_info.get_style_context()->add_class("dock-sub");
    info_box->pack_start(m_label_title, Gtk::PACK_SHRINK);
    info_box->pack_start(m_label_info, Gtk::PACK_SHRINK);
    m_dock_pills.set_halign(Gtk::ALIGN_START);
    info_box->pack_start(m_dock_pills, Gtk::PACK_SHRINK);
    m_details_box.pack_start(*info_box, Gtk::PACK_EXPAND_WIDGET);

    // Actions on the right (packed end -> Launch rightmost, then Artwork, then ★).
    m_button_play.set_valign(Gtk::ALIGN_CENTER);
    m_button_download_art.set_valign(Gtk::ALIGN_CENTER);
    m_button_favorite.set_valign(Gtk::ALIGN_CENTER);
    m_button_favorite.set_tooltip_text(_("Toggle favorite"));
    m_button_favorite.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_dock_favorite_clicked));
    m_details_box.pack_end(m_button_play, Gtk::PACK_SHRINK);
    m_details_box.pack_end(m_button_download_art, Gtk::PACK_SHRINK);
    m_details_box.pack_end(m_button_favorite, Gtk::PACK_SHRINK);

    // === Filter TreeView Setup ===
    m_model_filters = Gtk::TreeStore::create(m_filter_columns);
    m_treeview_filters.set_model(m_model_filters);
    
    // Create a single column with both icon and text
    auto combined_column = Gtk::manage(new Gtk::TreeView::Column("Filters"));
    
    // Add icon renderer
    auto icon_renderer = Gtk::manage(new Gtk::CellRendererPixbuf());
    combined_column->pack_start(*icon_renderer, false);
    combined_column->add_attribute(icon_renderer->property_pixbuf(), m_filter_columns.m_col_icon);
    icon_renderer->property_xpad() = 6;
    icon_renderer->property_ypad() = 5;

    // Add text renderer
    auto text_renderer = Gtk::manage(new Gtk::CellRendererText());
    combined_column->pack_start(*text_renderer, true);
    combined_column->add_attribute(text_renderer->property_text(), m_filter_columns.m_col_name);
    text_renderer->property_ypad() = 5; // taller, airier rows

    // Design-only: render group headers (roots/categories) in bold, like the
    // mockup's LIBRARY / SYSTEMS section labels. Filter logic is untouched.
    combined_column->set_cell_data_func(*text_renderer,
        [this, text_renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            if (!it) return;
            const auto& row = *it;
            std::string type = Glib::ustring(row[m_filter_columns.m_col_type]).raw();
            bool header = (type == "root" || type == "category");
            text_renderer->property_weight() = header ? Pango::WEIGHT_BOLD : Pango::WEIGHT_NORMAL;
        });

    m_treeview_filters.append_column(*combined_column);
    
    // Configure filter TreeView - clean look without lines
    m_treeview_filters.set_headers_visible(false);
    m_treeview_filters.set_enable_tree_lines(false);
    m_treeview_filters.set_show_expanders(true);
    m_treeview_filters.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_filter_selection_changed));
    
    m_scrolled_filters.add(m_treeview_filters);
    m_scrolled_filters.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scrolled_filters.set_size_request(250, -1); // Fixed width for filter panel
    
    // === Cover-art grid view (alternative to the list) ===
    m_flowbox.set_valign(Gtk::ALIGN_START);
    m_flowbox.set_selection_mode(Gtk::SELECTION_SINGLE);
    // Single click only selects (updates the detail dock); launching is on
    // double-click / Enter (child-activated).
    m_flowbox.set_activate_on_single_click(false);
    m_flowbox.set_homogeneous(true);
    m_flowbox.set_row_spacing(14);
    m_flowbox.set_column_spacing(14);
    m_flowbox.set_min_children_per_line(2);
    m_flowbox.set_max_children_per_line(12);
    m_flowbox.set_margin_top(12);
    m_flowbox.set_margin_bottom(12);
    m_flowbox.set_margin_start(12);
    m_flowbox.set_margin_end(12);
    m_flowbox.get_style_context()->add_class("game-grid");
    m_flowbox.signal_selected_children_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_grid_selection_changed));
    m_flowbox.signal_child_activated().connect(
        sigc::mem_fun(*this, &MainWindow::on_grid_child_activated));
    m_scrolled_grid.add(m_flowbox);
    m_scrolled_grid.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

    // Stack switches between the list and the grid.
    m_view_stack.add(m_scrolled_games, "list");
    m_view_stack.add(m_scrolled_grid, "grid");
    m_view_stack.set_visible_child("list");

    // === Layout: sidebar | (views on top, detail dock at bottom) ===
    m_right_box.pack_start(m_view_stack, Gtk::PACK_EXPAND_WIDGET);
    m_right_box.pack_start(m_details_box, Gtk::PACK_SHRINK);

    m_paned_main.pack1(m_scrolled_filters, false, false);
    m_paned_main.pack2(m_right_box, true, true);
    m_paned_main.set_position(250);

    // === Status Bar ===
    m_status_label.set_margin_end(10);
    m_status_label.set_halign(Gtk::ALIGN_END);
    m_status_label.set_size_request(400, -1);  // Largeur fixe pour le texte de scan

    // Configure thumbnail download progress widgets
    m_download_status_label.set_size_request(200, -1);  // Increased width for more characters
    m_download_status_label.set_ellipsize(Pango::ELLIPSIZE_END);  // Add ellipsis for long names
    m_download_status_label.set_halign(Gtk::ALIGN_START);
    
    m_download_progress_bar.set_size_request(200, 20);
    m_download_progress_bar.set_show_text(true);
    
    // Configure cancel button
    m_download_cancel_button.set_size_request(60, 25);
    
    m_download_progress_box.pack_start(m_download_status_label, Gtk::PACK_SHRINK);
    m_download_progress_box.pack_start(m_download_progress_bar, Gtk::PACK_SHRINK);
    m_download_progress_box.pack_start(m_download_cancel_button, Gtk::PACK_SHRINK);
    m_download_progress_box.set_spacing(3);  // Tighter spacing within download box
    m_download_progress_box.set_size_request(470, -1);  // Fixed total width: 200 + 200 + 60 + spacing
    m_download_progress_box.set_no_show_all(true);  // Prevent showing on parent show_all()
    m_download_progress_box.hide();  // Hidden by default
    
    // Scan progress widgets — hidden until a scan is running
    m_scan_progress_bar.set_size_request(160, 18);
    m_scan_progress_bar.set_show_text(false);
    m_scan_progress_label.set_size_request(220, -1);
    m_scan_progress_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_scan_progress_label.set_halign(Gtk::ALIGN_START);
    m_scan_details_button.set_size_request(90, 24);
    m_scan_details_button.signal_clicked().connect([this]() {
        if (m_scan_dialog) {
            m_scan_dialog->show();
            m_scan_dialog->present();
        }
    });
    m_scan_status_box.pack_start(m_scan_progress_label, Gtk::PACK_SHRINK);
    m_scan_status_box.pack_start(m_scan_progress_bar,   Gtk::PACK_SHRINK);
    m_scan_status_box.pack_start(m_scan_details_button, Gtk::PACK_SHRINK);
    m_scan_status_box.set_no_show_all(true);
    m_scan_status_box.hide();

    m_stats_box.set_halign(Gtk::ALIGN_START);
    m_stats_box.set_spacing(4);
    m_summary_label.get_style_context()->add_class("dim-label");
    m_status_box.pack_start(m_stats_box,              Gtk::PACK_EXPAND_WIDGET);
    m_status_box.pack_start(m_scan_status_box,        Gtk::PACK_SHRINK);
    m_status_box.pack_start(m_status_label,           Gtk::PACK_SHRINK);
    m_status_box.pack_start(m_download_progress_box,  Gtk::PACK_SHRINK);
    m_status_box.pack_end(m_summary_label,            Gtk::PACK_SHRINK);

    m_status_box.set_margin_start(6);
    m_status_box.set_margin_end(6);
    m_status_box.set_spacing(5);

    // === Header bar (modern client-side titlebar) ===
    // Brand on the left, centered search, and view/scan/settings/menu/language
    // actions on the right — matching the design mockup.
    m_headerbar.set_show_close_button(true);

    auto* brand = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    auto* logo = Gtk::make_managed<Gtk::Box>();
    logo->get_style_context()->add_class("brand-logo");
    logo->set_size_request(30, 30);
    logo->set_valign(Gtk::ALIGN_CENTER);
    auto* names = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    names->set_valign(Gtk::ALIGN_CENTER);
    auto* nm = Gtk::make_managed<Gtk::Label>();
    nm->set_markup("<b>FBNeo Launcher</b>");
    nm->set_xalign(0.0f);
    auto* subn = Gtk::make_managed<Gtk::Label>(_("Arcade library"));
    subn->set_xalign(0.0f);
    subn->get_style_context()->add_class("brand-sub");
    names->pack_start(*nm, Gtk::PACK_SHRINK);
    names->pack_start(*subn, Gtk::PACK_SHRINK);
    brand->pack_start(*logo, Gtk::PACK_SHRINK);
    brand->pack_start(*names, Gtk::PACK_SHRINK);
    m_headerbar.pack_start(*brand);

    m_menu_button.set_image_from_icon_name("open-menu-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_menu_button.set_tooltip_text(_("Menu"));
    m_app_menu.show_all();
    m_menu_button.set_popup(m_app_menu);

    m_btn_settings.set_image_from_icon_name("emblem-system-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_btn_settings.set_tooltip_text(_("Settings"));
    m_btn_settings.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_settings_clicked));

    populate_language_combo();
    m_lang_combo.set_tooltip_text(_("Language"));
    m_lang_combo.signal_changed().connect([this] {
        if (!m_suppress_lang_signal) on_language_selected(m_lang_combo.get_active_id());
    });

    auto* view_seg = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL);
    view_seg->get_style_context()->add_class("linked");
    view_seg->pack_start(m_btn_view_list);
    view_seg->pack_start(m_btn_view_grid);

    // Packed end -> rightmost first: language, menu, settings, scan, view toggle.
    m_headerbar.pack_end(m_lang_combo);
    m_headerbar.pack_end(m_menu_button);
    m_headerbar.pack_end(m_btn_settings);
    m_headerbar.pack_end(m_button_scan);
    m_headerbar.pack_end(*view_seg);

    set_titlebar(m_headerbar);
    m_headerbar.show_all();

    // === Packing ===
    m_main_box.pack_start(m_paned_main, Gtk::PACK_EXPAND_WIDGET);
    m_main_box.pack_start(m_status_box, Gtk::PACK_SHRINK);

    add(m_main_box);

    // === Load Database ===
    if (progress_callback) progress_callback(0.8, "Setting up game list...");
    
    m_model_games->clear();
    m_cached_games.clear();

    // Use preloaded games if provided, otherwise load from database
    std::vector<Game> db_games;
    if (!preloaded_games.empty()) {
        db_games = preloaded_games;
        std::cout << "[INFO] Using preloaded games - " << db_games.size() << " games available" << std::endl;
    } else {
        if (progress_callback) progress_callback(0.85, "Loading database...");
        db_games = m_database->getAllGames();
        
        if (db_games.empty()) {
            std::cout << "[INFO] Database is empty - use 'Update DAT' button to load games" << std::endl;
            m_status_label.set_text("Database empty - use 'Update DAT' button to load games");
            m_status_label.show();
        } else {
            std::cout << "[INFO] Database loaded - " << db_games.size() << " games available" << std::endl;
        }
    }
    
    // Keep compatibility with legacy code - load games into cache
    m_cached_games = db_games;
    
    // Populate system filter and display games
    if (!m_cached_games.empty()) {
        if (progress_callback) progress_callback(0.9, "Loading filter cache...");
        
        // Load filter cache (will be empty if no DAT update has been done yet)
        load_filter_cache();
        
        if (progress_callback) progress_callback(0.92, "Populating filters...");
        
        // Removed all ComboBox filter initialization - will implement MAMEUI-style panel
        
        if (progress_callback) progress_callback(0.95, "Finalizing interface...");
        
        // Populate filter tree and defer game filtering
        Glib::signal_idle().connect_once([this]() {
            populate_filter_tree();
            filter_games();
            update_status_bar_stats();
        });
    }

    // === Signals ===
    m_toolbar_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));
    m_button_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));
    m_button_download_art.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_download_art_clicked));
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_start_scan_clicked));
    m_button_update_dat.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_update_dat_clicked));
    m_download_cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_download_cancel_clicked));
    
    // Connect preview and titles download buttons from settings panel
    m_settings_panel.get_download_previews_button().signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_download_previews_clicked));
    m_settings_panel.get_download_titles_button().signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_download_titles_clicked));
    
    // Connect thumbnail download dispatchers
    m_download_progress_dispatcher.connect([this]() {
        show_download_progress(m_current_download_file, m_current_download_index, 
                              m_total_download_count, m_download_percentage);
    });
    
    m_download_finished_dispatcher.connect([this]() {
        hide_download_progress();
        std::cout << "[INFO] All artwork downloaded!" << std::endl;
        
        // Update status bar with completion message instead of popup
        m_status_label.set_text("Download completed successfully!");
        
        // Hide the completion message after 5 seconds
        Glib::signal_timeout().connect([this]() {
            m_status_label.set_text("");
            return false; // Don't repeat the timeout
        }, 5000);
    });
    
    // Connect ROM scan dispatchers
    m_scan_progress_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_scan_progress));
    m_scan_finished_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_scan_finished));
    
    // Removed filter population dispatcher

    // === Final setup ===
    if (progress_callback) progress_callback(0.95, "Finalizing interface...");
    show_all_children();
    
    if (progress_callback) progress_callback(1.0, "Ready!");
    std::cout << "[DEBUG] MainWindow constructor completed" << std::endl;
}

MainWindow::~MainWindow() {
    // Clean up scan thread
    if (m_scan_thread.joinable()) {
        m_scan_cancelled = true;
        m_scan_thread.join();
    }
    
    // Disconnect timeout connection
    if (m_search_timeout_connection.connected()) {
        m_search_timeout_connection.disconnect();
    }
}

void MainWindow::on_game_selected() {
    auto selection = m_treeview_games.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;

    Gtk::TreeModel::Row row = *iter;
    std::string name = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    
    // Get system prefix for file lookup
    std::string system_prefix = "";
    // We need to access the ThumbnailDownloader method, but it's private
    // For now, let's duplicate the logic here (not ideal, but simpler)
    if (system.find("Fairchild_Channel_F") != std::string::npos) {
        system_prefix = "chf_";
    } else if (system.find("ColecoVision") != std::string::npos) {
        system_prefix = "cv_";
    } else if (system.find("Sega_Game_Gear") != std::string::npos) {
        system_prefix = "gg_";
    } else if (system.find("MegaDrive") != std::string::npos) {
        system_prefix = "md_";
    } else if (system.find("TurboGrafx-16") != std::string::npos) {
        system_prefix = "tg_";
    } else if (system.find("MSX") != std::string::npos) {
        system_prefix = "msx_";
    } else if (system.find("Sega_Master_System") != std::string::npos) {
        system_prefix = "sms_";
    } else if (system.find("Nintendo_Entertainment_System") != std::string::npos) {
        system_prefix = "nes_";
    } else if (system.find("Neo_Geo_Pocket") != std::string::npos) {
        system_prefix = "ngp_";
    } else if (system.find("PC_ENGINE") != std::string::npos) {
        system_prefix = "pce_";
    } else if (system.find("Nintendo_Famicom_Disk_System") != std::string::npos) {
        system_prefix = "fds_";
    } else if (system.find("Super_Nintendo_Entertainment_System") != std::string::npos) {
        system_prefix = "snes_";
    } else if (system.find("Sinclair_ZX_Spectrum") != std::string::npos) {
        system_prefix = "spec_";
    } else if (system.find("Sega_SG-1000") != std::string::npos) {
        system_prefix = "sg1k_";
    } else if (system.find("PC_Engine_SuperGrafx") != std::string::npos) {
        system_prefix = "sgx_";
    }
    // Arcade et Neo Geo n'ont pas de préfixe
    
    // Load preview image - use ROM name with system prefix for file lookup
    std::string previews_path = m_settings_panel.get_previews_path();
    std::string filename_with_prefix = system_prefix + name;
    std::string preview_image_path = previews_path + "/" + filename_with_prefix + ".png";

    try {
        if (!previews_path.empty() && std::filesystem::exists(preview_image_path)) {
            Glib::RefPtr<Gdk::Pixbuf> pixbuf = Gdk::Pixbuf::create_from_file(preview_image_path);
            if (pixbuf) {
                m_preview_image.set(pixbuf->scale_simple(128, 96, Gdk::INTERP_BILINEAR));
                m_preview_image.show();
            } else {
                m_preview_image.hide();
            }
        } else {
            m_preview_image.hide();
        }
    } catch (...) {
        m_preview_image.hide();
    }

    // Load title image - use ROM name with system prefix for file lookup
    std::string titles_path = m_settings_panel.get_titles_path();
    std::string title_image_path = titles_path + "/" + filename_with_prefix + ".png";

    try {
        if (!titles_path.empty() && std::filesystem::exists(title_image_path)) {
            Glib::RefPtr<Gdk::Pixbuf> pixbuf = Gdk::Pixbuf::create_from_file(title_image_path);
            if (pixbuf) {
                m_title_image.set(pixbuf->scale_simple(300, 100, Gdk::INTERP_BILINEAR));
                m_title_image.show();
            } else {
                m_title_image.hide();
            }
        } else {
            m_title_image.hide();
        }
    } catch (...) {
        m_title_image.hide();
    }

    std::string manufacturer = Glib::ustring(row[m_columns.m_col_manufacturer]).raw();
    std::string year         = Glib::ustring(row[m_columns.m_col_year]).raw();
    std::string status       = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool        fav          = row[m_columns.m_col_favorite];

    m_label_title.set_markup("<b>" + escape_markup(title.empty() ? name : title) + "</b>");

    // Subtitle: System · Manufacturer · Year (skip empty parts).
    std::vector<std::string> parts;
    if (!system.empty())       parts.push_back(system);
    if (!manufacturer.empty()) parts.push_back(manufacturer);
    if (!year.empty())         parts.push_back(year);
    std::string sub;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) sub += "  ·  "; sub += parts[i]; }
    m_label_info.set_text(sub);

    // Pills: status / zip / CRC (matches the design mockup).
    for (auto* c : m_dock_pills.get_children()) m_dock_pills.remove(*c);
    auto add_pill = [this](const std::string& text, const char* cls) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->get_style_context()->add_class("pill");
        if (cls) l->get_style_context()->add_class(cls);
        m_dock_pills.pack_start(*l, Gtk::PACK_SHRINK);
    };
    if (status == "available") {
        add_pill("● " + _("Available"), "pill-ok");
        add_pill(name + ".zip", nullptr);
        add_pill(_("ROM verified (CRC)"), nullptr);
    } else if (status == "incorrect") {
        add_pill("● " + _("Incorrect"), "pill-warn");
        add_pill(name + ".zip", nullptr);
        add_pill(_("CRC mismatch"), nullptr);
    } else {
        add_pill("● " + _("Missing"), "pill-muted");
    }
    m_dock_pills.show_all();

    m_button_favorite.set_label(fav ? "★" : "☆");
    m_button_favorite.set_sensitive(true);
    m_button_play.set_sensitive(true); // Details panel button
    m_button_download_art.set_sensitive(true); // Download Art button
    m_toolbar_play.set_sensitive(true); // Toolbar button
}

void MainWindow::on_dock_favorite_clicked() {
    auto iter = m_treeview_games.get_selection()->get_selected();
    if (!iter) return;
    Gtk::TreeModel::Row row = *iter;
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    m_database->toggleFavorite(name, system);
    bool now_fav = m_database->isFavorite(name, system);
    row[m_columns.m_col_favorite] = now_fav;
    m_button_favorite.set_label(now_fav ? "★" : "☆");
    for (auto& g : m_cached_games)
        if (g.name == name && g.system == system) { g.is_favorite = now_fav; break; }
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
        Gtk::MessageDialog dlg(*this, "FBNeo not configured", false, Gtk::MESSAGE_ERROR);
        dlg.set_secondary_text("Please set the FBNeo executable path in Settings.");
        dlg.run();
        return;
    }

    // Verify the executable exists and is actually executable
    {
        std::error_code ec;
        auto status_fs = std::filesystem::status(fbneo_executable, ec);
        if (ec || !std::filesystem::exists(status_fs)) {
            Gtk::MessageDialog dlg(*this, "FBNeo executable not found", false, Gtk::MESSAGE_ERROR);
            dlg.set_secondary_text("The file does not exist:\n" + fbneo_executable
                                   + "\n\nPlease update the path in Settings.");
            dlg.run();
            return;
        }
        if (access(fbneo_executable.c_str(), X_OK) != 0) {
            Gtk::MessageDialog dlg(*this, "FBNeo not executable", false, Gtk::MESSAGE_ERROR);
            dlg.set_secondary_text("The file exists but is not executable:\n" + fbneo_executable
                                   + "\n\nRun: chmod +x \"" + fbneo_executable + "\"");
            dlg.run();
            return;
        }
    }

    if (roms_paths.empty()) {
        Gtk::MessageDialog dlg(*this, "No ROM directories configured", false, Gtk::MESSAGE_ERROR);
        dlg.set_secondary_text("Please add at least one ROM directory in Settings.");
        dlg.run();
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
    
    // === Verify ZIP integrity before launching ===
    {
        std::string zip_path = find_rom_zip_path(rom_name);
        if (!zip_path.empty() && !verify_zip_integrity(zip_path)) {
            Gtk::MessageDialog dlg(*this, "Corrupt ROM archive", false, Gtk::MESSAGE_WARNING,
                                   Gtk::BUTTONS_OK_CANCEL, true);
            dlg.set_secondary_text("The ZIP file appears corrupt:\n" + zip_path
                                   + "\n\nLaunch anyway?");
            if (dlg.run() != Gtk::RESPONSE_OK) return;
        }
    }

    // === Build launch args ===
    std::vector<std::string> launch_args;
    launch_args.push_back(fbneo_executable);
    launch_args.push_back("-joy");                          // always enable joystick
    if (m_launch_fullscreen)   launch_args.push_back("-fullscreen");
    if (m_launch_integerscale) launch_args.push_back("-integerscale");
    launch_args.push_back(fbneo_rom_name);

    std::cout << "Launching " << game_system << " game:";
    for (const auto& a : launch_args) std::cout << " " << a;
    std::cout << std::endl;

    // Record launch (last_played + play_count)
    m_database->recordLaunch(rom_name, game_system);

    pid_t pid = spawn_process(launch_args);
    if (pid > 0) {
        // Detached watcher thread: waits for process exit and records playtime
        std::thread([this, pid, rom_name, game_system]() {
            watch_playtime(pid, m_database, rom_name, game_system);
        }).detach();
    }
}

void MainWindow::on_download_art_clicked() {
    auto selection = m_treeview_games.get_selection();
    auto iter = selection->get_selected();
    if (!iter) {
        return;
    }

    // Récupérer les informations du jeu sélectionné
    Gtk::TreeModel::Row row = *iter;
    std::string game_name = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string game_title = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string game_system = Glib::ustring(row[m_columns.m_col_system]).raw();
    
    // Vérifier que les répertoires sont configurés
    std::string previews_dir = m_settings_panel.get_previews_path();
    std::string titles_dir = m_settings_panel.get_titles_path();
    
    if (previews_dir.empty() && titles_dir.empty()) {
        Gtk::MessageDialog dialog(*this, "Artwork Directories Not Set", false, Gtk::MESSAGE_WARNING);
        dialog.set_secondary_text("Please set the previews and/or titles directories in Settings before downloading.");
        dialog.run();
        return;
    }
    
    // Vérifier si un téléchargement est déjà en cours
    if (m_thumbnail_downloader.is_downloading()) {
        Gtk::MessageDialog dialog(*this, "Download In Progress", false, Gtk::MESSAGE_INFO);
        dialog.set_secondary_text("Artwork download is already in progress.");
        dialog.run();
        return;
    }
    
    std::cout << "[INFO] Downloading artwork for: " << game_title << std::endl;
    
    // Create callback to update status label for single downloads
    auto single_download_callback = [this](const std::string& status, int current, int total, double percentage) {
        m_status_label.set_text(status);
    };
    
    // Download preview first if directory is configured - use ROM name and system
    if (!previews_dir.empty()) {
        std::cout << "[INFO] Downloading preview for: " << game_title << " (ROM: " << game_name << ", System: " << game_system << ")" << std::endl;
        m_status_label.set_text("Downloading preview for " + game_title + "...");
        m_thumbnail_downloader.download_single_artwork(game_name, game_system, previews_dir, ThumbnailDownloader::ArtworkType::Previews, single_download_callback);
        
        // Wait a moment before downloading title
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // Download title if directory is configured - use ROM name and system
    if (!titles_dir.empty() && !m_thumbnail_downloader.is_downloading()) {
        std::cout << "[INFO] Downloading title for: " << game_title << " (ROM: " << game_name << ", System: " << game_system << ")" << std::endl;
        m_status_label.set_text("Downloading title for " + game_title + "...");
        m_thumbnail_downloader.download_single_artwork(game_name, game_system, titles_dir, ThumbnailDownloader::ArtworkType::Titles, single_download_callback);
    }
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
    std::cout << "[INFO] Starting ROM scan using database" << std::endl;
    
    // Confirmation dialog with custom styling
    ConfirmationDialog confirm_dialog(*this, 
        "Warning: Scan ROMs",
        "This will rescan all ROM directories to update game status.\nThis process can take several minutes depending on your ROM collection.\n\nAre you sure you want to continue?",
        "⚠️");
    
    if (!confirm_dialog.show_and_confirm()) {
        std::cout << "[INFO] ROM scan cancelled by user" << std::endl;
        return;
    }
    
    // Prevent multiple scans from running
    if (m_scan_in_progress) {
        std::cout << "[WARNING] Scan already in progress" << std::endl;
        return;
    }
    
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
    
    // Ensure database is loaded with DAT data (cheap count query, no full load)
    if (m_database->getGameCount() == 0) {
        std::string dat_path = m_settings_panel.get_dat_path();
        if (dat_path.empty()) {
            m_status_label.set_text("Error: No DAT path defined");
            m_status_label.show();
            return;
        }
        
        std::cout << "[INFO] Reloading DAT files to database..." << std::endl;
        if (!DatParser::parseAllDatsToDatabase(dat_path, m_database)) {
            m_status_label.set_text("Error: Failed to load DAT files");
            m_status_label.show();
            return;
        }
    }
    
    // Start threaded scan
    start_scan_thread(roms_paths);
}

void MainWindow::on_update_dat_clicked() {
    std::cout << "[INFO] Update DAT requested" << std::endl;
    
    // Confirmation dialog with custom styling
    ConfirmationDialog confirm_dialog(*this,
        "Update DAT",
        "The game database will be reloaded from the DAT files.\nGames whose ROM definition is unchanged keep their status —\nonly new or changed games are re-checked on the next scan.\n\nDo you want to continue?",
        "🔄");
    
    if (!confirm_dialog.show_and_confirm()) {
        std::cout << "[INFO] Update DAT cancelled by user" << std::endl;
        return;
    }
    
    std::string dat_path = m_settings_panel.get_dat_path();
    if (dat_path.empty()) {
        Gtk::MessageDialog dialog(*this, "Error", false, Gtk::MESSAGE_ERROR);
        dialog.set_secondary_text("No DAT path configured. Please configure the path in settings.");
        dialog.run();
        return;
    }
    
    // Create and show the update dialog
    DATUpdateDialog dialog(*this, m_database, dat_path);
    dialog.start_update();
    
    int result = dialog.run();
    
    if (!dialog.was_cancelled()) {
        // Reload games from database and refresh interface
        std::cout << "[INFO] Reloading games after DAT update..." << std::endl;
        m_cached_games = m_database->getAllGames();
        
        // Regenerate filter cache from updated games
        std::cout << "[INFO] Regenerating filter cache after DAT update..." << std::endl;
        m_filter_cache = FilterCache::generate_from_games(m_cached_games);
        save_filter_cache();
        m_filter_cache_loaded = true; // Mark as loaded with new data
        
        // Filter cache updated - will be used by future MAMEUI-style filter panel
        std::cout << "[INFO] Filter cache updated with new DAT data" << std::endl;
        
        // Refresh display
        filter_games();
        update_status_bar_stats();
        
        std::cout << "[INFO] Interface updated with " << m_cached_games.size() << " games" << std::endl;
    }
}


void MainWindow::on_settings_clicked() {
    auto dialog = Gtk::Dialog("Settings", *this, Gtk::DIALOG_MODAL);
    dialog.set_default_size(800, 500);
    dialog.get_content_area()->pack_start(m_settings_panel);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("OK", Gtk::RESPONSE_OK);

    m_settings_panel.show();

    // Connect signal to close dialog when download starts
    sigc::connection close_connection = m_close_settings_signal.connect([&dialog]() {
        dialog.response(Gtk::RESPONSE_OK);
    });

    int result = dialog.run();
    
    // Disconnect the signal after dialog closes
    close_connection.disconnect();
    
    if (result == Gtk::RESPONSE_OK) {
        m_settings_panel.save_to_file(AppContext::get_config_path());
    }
}

void MainWindow::on_hide() {
    m_settings_panel.save_to_file(AppContext::get_config_path());
    save_launch_prefs();
    Gtk::Window::on_hide();
}

void MainWindow::on_quit() {
    m_settings_panel.save_to_file(AppContext::get_config_path());
    save_launch_prefs();
    Gtk::Main::quit();
}

void MainWindow::update_status_bar_stats() {
    int total = 0, available = 0, incorrect = 0, missing = 0, error = 0;

    // Count stats from filtered games (much faster than re-filtering)
    std::lock_guard<std::mutex> lock(m_filter_mutex);
    for (const auto& game : m_filtered_games) {
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

    // Legend with coloured dots, matching the detail-dock / grid status colours.
    auto add_stat = [&](const char* color, int count, const std::string& label) {
        auto* l = Gtk::make_managed<Gtk::Label>();
        l->set_markup("<span foreground=\"" + std::string(color) + "\">●</span> " +
                      Glib::Markup::escape_text(label) + " <b>" + std::to_string(count) + "</b>");
        l->set_margin_end(14);
        m_stats_box.pack_start(*l, Gtk::PACK_SHRINK);
    };
    add_stat("#41d08a", available, _("Available"));
    add_stat("#f0b54a", incorrect, _("Incorrect"));
    add_stat("#5a6272", missing,   _("Missing"));
    if (error > 0) add_stat("#ff6f6f", error, _("Error"));

    // Explicit total ROM count.
    auto* tot = Gtk::make_managed<Gtk::Label>();
    tot->set_markup("· " + Glib::Markup::escape_text(_("Total")) + " <b>" + std::to_string(total) + "</b>");
    m_stats_box.pack_start(*tot, Gtk::PACK_SHRINK);
    m_stats_box.show_all();

    // Right-aligned summary: "N available / Total".
    m_summary_label.set_markup("<b>" + std::to_string(available) + "</b> " +
                               Glib::Markup::escape_text(_("available")) +
                               " / <b>" + std::to_string(total) + "</b>");
    m_summary_label.show();
}

// Removed all ComboBox filter handlers - will implement MAMEUI-style filtering

void MainWindow::filter_games() {
    filter_games_simple();
}

void MainWindow::filter_games_async() {
    // For search, use debouncing to avoid excessive filtering
    if (m_search_timeout_connection.connected()) {
        m_search_timeout_connection.disconnect();
    }
    
    m_search_timeout_connection = Glib::signal_timeout().connect([this]() {
        filter_games_simple();
        return false;
    }, 200); // 200ms debounce for search
}

void MainWindow::filter_games_simple() {
    // Use the new tree-based filtering system
    apply_tree_filters();
}

void MainWindow::apply_filters() {
    // Apply filtered results to TreeView in main thread.
    // Take a snapshot under the lock, then release before touching the TreeView
    // or calling update_status_bar_stats() (which also acquires m_filter_mutex).
    std::vector<Game> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        snapshot = m_filtered_games;
    }

    // Detach model + disable sort to avoid one redraw and one re-sort per insert.
    m_treeview_games.unset_model();
    int prev_sort_col = Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID;
    Gtk::SortType prev_sort_order = Gtk::SORT_ASCENDING;
    bool had_sort = m_model_games->get_sort_column_id(prev_sort_col, prev_sort_order);
    m_model_games->set_sort_column(Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID, Gtk::SORT_ASCENDING);
    m_model_games->clear();

    for (const auto& game : snapshot) {
        auto row = *m_model_games->append();
        row[m_columns.m_col_icon]     = IconManager::get_status_icon(game.status);
        row[m_columns.m_col_status]   = game.status;   // needed by the detail dock pills
        row[m_columns.m_col_favorite] = game.is_favorite;
        row[m_columns.m_col_name]     = game.name;
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

    if (had_sort &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID) {
        m_model_games->set_sort_column(prev_sort_col, prev_sort_order);
    }
    m_treeview_games.set_model(m_model_games);

    // Keep the cover grid in sync when it is the active view.
    if (m_view_stack.get_visible_child_name() == "grid")
        rebuild_grid();

    update_status_bar_stats();
}

void MainWindow::set_view_mode(bool grid) {
    m_suppress_view_toggle = true;
    m_btn_view_grid.set_active(grid);
    m_btn_view_list.set_active(!grid);
    m_suppress_view_toggle = false;
    if (grid) rebuild_grid();               // build lazily, only when shown
    m_view_stack.set_visible_child(grid ? "grid" : "list");
}

std::string MainWindow::resolve_preview_path(const std::string& name, const std::string& system) {
    static const std::vector<std::pair<const char*, const char*>> prefixes = {
        {"Fairchild_Channel_F","chf_"}, {"ColecoVision","cv_"}, {"Sega_Game_Gear","gg_"},
        {"MegaDrive","md_"}, {"TurboGrafx-16","tg_"}, {"MSX","msx_"}, {"Sega_Master_System","sms_"},
        {"Nintendo_Entertainment_System","nes_"}, {"Neo_Geo_Pocket","ngp_"}, {"PC_ENGINE","pce_"},
        {"Nintendo_Famicom_Disk_System","fds_"}, {"Super_Nintendo_Entertainment_System","snes_"},
        {"Sinclair_ZX_Spectrum","spec_"}, {"Sega_SG-1000","sg1k_"}, {"PC_Engine_SuperGrafx","sgx_"}};
    std::string pfx;
    for (const auto& [key, p] : prefixes)
        if (system.find(key) != std::string::npos) { pfx = p; break; }

    for (const std::string& dir : {m_settings_panel.get_previews_path(), m_settings_panel.get_titles_path()}) {
        if (dir.empty()) continue;
        std::string path = dir + "/" + pfx + name + ".png";
        if (std::filesystem::exists(path)) return path;
    }
    return "";
}

Gtk::Widget* MainWindow::make_game_card(const Gtk::TreeModel::Row& row) {
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string status = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool fav = row[m_columns.m_col_favorite];

    const char* dot = status == "available" ? "#41d08a"
                    : status == "incorrect" ? "#f0b54a"
                    : status == "missing"   ? "#5a6272" : "#939aab";

    auto* card = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    card->get_style_context()->add_class("game-card");

    // Artwork (preview/title) or a titled placeholder.
    std::string art = resolve_preview_path(name, system);
    Gtk::Widget* art_w = nullptr;
    if (!art.empty()) {
        try {
            auto pix = Gdk::Pixbuf::create_from_file(art, 176, 132, true);
            auto* img = Gtk::make_managed<Gtk::Image>(pix);
            img->get_style_context()->add_class("card-art");
            art_w = img;
        } catch (...) {}
    }
    if (!art_w) {
        auto* ph = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
        ph->get_style_context()->add_class("card-art");
        ph->get_style_context()->add_class("card-art-empty");
        ph->set_size_request(176, 132);
        auto* l = Gtk::make_managed<Gtk::Label>(name);
        l->set_line_wrap(true);
        l->set_justify(Gtk::JUSTIFY_CENTER);
        l->set_max_width_chars(14);
        l->set_valign(Gtk::ALIGN_CENTER);
        l->set_vexpand(true);
        l->get_style_context()->add_class("card-art-title");
        ph->pack_start(*l, true, true);
        art_w = ph;
    }
    card->pack_start(*art_w, Gtk::PACK_SHRINK);

    auto* meta = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
    meta->get_style_context()->add_class("card-meta");
    auto* nlbl = Gtk::make_managed<Gtk::Label>();
    nlbl->set_text((fav ? "★ " : "") + name);
    nlbl->set_ellipsize(Pango::ELLIPSIZE_END);
    nlbl->set_xalign(0.0f);
    nlbl->get_style_context()->add_class("card-name");
    meta->pack_start(*nlbl, Gtk::PACK_SHRINK);
    auto* slbl = Gtk::make_managed<Gtk::Label>();
    slbl->set_markup("<span foreground=\"" + std::string(dot) + "\">●</span> " +
                     Glib::Markup::escape_text(system));
    slbl->set_ellipsize(Pango::ELLIPSIZE_END);
    slbl->set_xalign(0.0f);
    slbl->get_style_context()->add_class("card-sys");
    meta->pack_start(*slbl, Gtk::PACK_SHRINK);
    card->pack_start(*meta, Gtk::PACK_SHRINK);

    return card;
}

void MainWindow::rebuild_grid() {
    for (auto* c : m_flowbox.get_children()) m_flowbox.remove(*c);
    m_grid_refs.clear();

    auto children = m_model_games->children();
    size_t built = 0;
    for (auto row : children) {
        if (built >= static_cast<size_t>(m_grid_cap)) break;
        Gtk::Widget* card = make_game_card(row);
        m_grid_refs.push_back(Gtk::TreeRowReference(m_model_games, m_model_games->get_path(row)));
        m_flowbox.add(*card);
        ++built;
    }
    m_flowbox.show_all_children();
}

void MainWindow::on_grid_selection_changed() {
    auto sel = m_flowbox.get_selected_children();
    if (sel.empty()) return;
    int idx = sel[0]->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_grid_refs.size())) return;
    auto& ref = m_grid_refs[idx];
    if (!ref.is_valid()) return;
    // Drive the treeview selection so on_game_selected() populates the details.
    m_treeview_games.get_selection()->select(ref.get_path());
}

void MainWindow::on_grid_child_activated(Gtk::FlowBoxChild* child) {
    if (!child) return;
    int idx = child->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_grid_refs.size())) return;
    auto& ref = m_grid_refs[idx];
    if (!ref.is_valid()) return;
    m_treeview_games.get_selection()->select(ref.get_path());
    on_play_clicked();
}

void MainWindow::configure_columns() {
    // Get all columns to configure them
    auto columns = m_treeview_games.get_columns();

    // Map each VIEW column (in append order) to its backing MODEL column. The view
    // omits the model's `status` column, so a view index is NOT the same as its
    // model column id — deriving the sort id from the loop index made "Type" and
    // every column after it sort by the wrong data (status, etc.).
    const std::vector<const Gtk::TreeModelColumnBase*> sort_cols = {
        nullptr,                        // 0  icon (not sortable)
        &m_columns.m_col_favorite,      // 1  ★
        &m_columns.m_col_name,          // 2  Name
        &m_columns.m_col_title,         // 3  Title
        &m_columns.m_col_year,          // 4  Year
        &m_columns.m_col_manufacturer,  // 5  Manufacturer
        &m_columns.m_col_system,        // 6  System
        &m_columns.m_col_video_type,    // 7  Type
        &m_columns.m_col_orientation,   // 8  Orientation
        &m_columns.m_col_width,         // 9  Width
        &m_columns.m_col_height,        // 10 Height
        &m_columns.m_col_aspect,        // 11 Aspect
        &m_columns.m_col_driver_status, // 12 Driver
        &m_columns.m_col_comment,       // 13 Comment
        &m_columns.m_col_cloneof,       // 14 Clone
        &m_columns.m_col_sourcefile,    // 15 Source
    };

    // Configure each column with proper sorting and sizing
    for (size_t i = 0; i < columns.size(); ++i) {
        auto column = columns[i];
        
        // Disable reordering to prevent GTK layout issues
        column->set_reorderable(false);
        
        // Enable resizing
        column->set_resizable(true);
        
        // Enable sorting (clickable headers)
        column->set_clickable(true);

        // Bind the header to the CORRECT model column so it sorts its own data.
        if (i < sort_cols.size() && sort_cols[i] != nullptr) {
            column->set_sort_column(*sort_cols[i]);
        }
        
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
            column->set_min_width(50);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(120);
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
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;";  break;
            case '>': escaped += "&gt;";  break;
            default:  escaped += c;       break;
        }
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
    std::cout << "Opening FBNeo menu: " << fbneo_executable << std::endl;
    spawn_process({fbneo_executable, "-menu"});
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
    std::string cfg_path = AppContext::get_config_path();
    // Always reload from file so dialog reflects any external changes
    ControllerManager::load_profiles(m_controller_profiles, m_active_controller_profile, cfg_path);

    ControllerDialog dlg(*this, m_controller_profiles, m_active_controller_profile, cfg_path);
    dlg.run();

    // Reload after dialog closes (in case user saved new profiles)
    ControllerManager::load_profiles(m_controller_profiles, m_active_controller_profile, cfg_path);
}

void MainWindow::on_fullscreen_mode() {
    m_launch_fullscreen = m_menu_item_fullscreen_mode.get_active();
    save_launch_prefs();
}

void MainWindow::on_integerscale_mode() {
    m_launch_integerscale = m_menu_item_integerscale_mode.get_active();
    save_launch_prefs();
}

void MainWindow::on_arcade_mode() {
    // Filter to show only arcade systems
    m_active_filters.clear();
    m_active_filters["system"] = "Arcade";
    apply_tree_filters();
    std::cout << "Filtering to arcade systems only" << std::endl;
}

void MainWindow::on_console_mode() {
    // Filter to show only console systems (all non-arcade systems)
    m_active_filters.clear();
    // Set a special filter for console mode - will exclude arcade
    m_active_filters["mode"] = "console";
    apply_tree_filters();
    std::cout << "Filtering to console systems only" << std::endl;
}

void MainWindow::on_all_systems() {
    // Show all systems - clear all filters
    m_active_filters.clear();
    apply_tree_filters();
    std::cout << "Showing all systems" << std::endl;
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
    m_active_filters.clear();
    m_active_filters["status"] = "available";
    apply_tree_filters();
    std::cout << "Filtering to show available ROMs only" << std::endl;
}

void MainWindow::on_show_missing_roms() {
    // Filter to show missing ROMs
    m_active_filters.clear();
    m_active_filters["status"] = "missing";
    apply_tree_filters();
    std::cout << "Filtering to show missing ROMs only" << std::endl;
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
        spawn_process({"xdg-open", "https://github.com/finalburnneo/FBNeo"});
    });
    button_box->pack_start(*github_button);
    
    // License button
    auto license_button = Gtk::manage(new Gtk::Button("📜 View License"));
    license_button->signal_clicked().connect([fbneo_dir]() {
        std::string license_path = fbneo_dir + "/license.txt";
        if (std::filesystem::exists(license_path)) {
            spawn_process({"xdg-open", license_path});
        }
    });
    button_box->pack_start(*license_button);
    
    // What's New button
    auto whatsnew_button = Gtk::manage(new Gtk::Button("🆕 What's New"));
    whatsnew_button->signal_clicked().connect([fbneo_dir]() {
        std::string whatsnew_path = fbneo_dir + "/whatsnew.html";
        if (std::filesystem::exists(whatsnew_path)) {
            spawn_process({"xdg-open", whatsnew_path});
        }
    });
    button_box->pack_start(*whatsnew_button);
    
    // Help file button
    auto help_button = Gtk::manage(new Gtk::Button("❓ Help Documentation"));
    help_button->signal_clicked().connect([fbneo_dir]() {
        std::string help_path = fbneo_dir + "/fbneo.chm";
        if (std::filesystem::exists(help_path)) {
            spawn_process({"xdg-open", help_path});
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

// === Thumbnail Download Methods ===

void MainWindow::show_download_progress(const std::string& filename, int current, int total, double percentage) {
    // Update progress bar
    m_download_progress_bar.set_fraction(percentage / 100.0);
    m_download_progress_bar.set_text(std::to_string(static_cast<int>(percentage)) + "%");
    
    // Update status label with filename and count
    std::string status_text = "Downloading: " + filename + " (" + 
                             std::to_string(current) + "/" + std::to_string(total) + ")";
    m_download_status_label.set_text(status_text);
    
    // Show the download progress box if not visible
    if (!m_download_progress_box.get_visible()) {
        m_download_status_label.show();
        m_download_progress_bar.show();
        m_download_cancel_button.show();
        m_download_progress_box.show();
        // Hide main status label during mass downloads to avoid confusion
        m_status_label.hide();
    }
    
    // Force UI update
    while (Gtk::Main::events_pending()) {
        Gtk::Main::iteration();
    }
}

void MainWindow::hide_download_progress() {
    m_download_progress_box.hide();
    // Show main status label again after mass download completes
    m_status_label.show();
    // Clear the main status bar or show completion message
    update_status_bar_stats();
}

void MainWindow::on_download_previews_clicked() {
    std::string previews_dir = m_settings_panel.get_previews_path();
    
    // Vérifier que le répertoire de previews est configuré
    if (previews_dir.empty()) {
        Gtk::MessageDialog dialog(*this, "Previews Directory Not Set", false, Gtk::MESSAGE_WARNING);
        dialog.set_secondary_text("Please set the previews directory in Settings before downloading.");
        dialog.run();
        return;
    }
    
    // Vérifier qu'il y a des jeux chargés
    if (m_cached_games.empty()) {
        Gtk::MessageDialog dialog(*this, "No Games Loaded", false, Gtk::MESSAGE_WARNING);
        dialog.set_secondary_text("Please load or scan games before downloading previews.");
        dialog.run();
        return;
    }
    
    // Vérifier si un téléchargement est déjà en cours
    if (m_thumbnail_downloader.is_downloading()) {
        Gtk::MessageDialog dialog(*this, "Download In Progress", false, Gtk::MESSAGE_INFO);
        dialog.set_secondary_text("Preview download is already in progress.");
        dialog.run();
        return;
    }
    
    // Demander confirmation à l'utilisateur
    Gtk::MessageDialog confirm_dialog(*this, "Download Previews", false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
    confirm_dialog.set_secondary_text(
        "This will download previews for " + std::to_string(m_cached_games.size()) + 
        " games from FBNeo-extras.\n\nThis may take several minutes. Continue?"
    );
    
    if (confirm_dialog.run() != Gtk::RESPONSE_YES) {
        return;
    }
    
    // Close settings dialog if it's open
    m_close_settings_signal.emit();
    
    std::cout << "[INFO] Starting previews download for " << m_cached_games.size() << " games" << std::endl;
    
    // Créer le callback de progression
    auto progress_callback = [this](const std::string& filename, int current, int total, double percentage) {
        std::cout << "[DEBUG] Progress callback called: " << filename << " - " << percentage << "%" << std::endl;
        
        // Mettre à jour les variables partagées
        m_current_download_file = filename;
        m_current_download_index = current;
        m_total_download_count = total;
        m_download_percentage = percentage;
        
        // Déclencher le dispatcher approprié
        if (percentage >= 100.0) {
            m_download_finished_dispatcher.emit();
        } else {
            m_download_progress_dispatcher.emit();
        }
    };
    
    // Démarrer le téléchargement
    m_thumbnail_downloader.start_download(m_cached_games, previews_dir, ThumbnailDownloader::ArtworkType::Previews, progress_callback);
}

void MainWindow::on_download_titles_clicked() {
    std::string titles_dir = m_settings_panel.get_titles_path();
    
    // Vérifier que le répertoire de titles est configuré
    if (titles_dir.empty()) {
        Gtk::MessageDialog dialog(*this, "Titles Directory Not Set", false, Gtk::MESSAGE_WARNING);
        dialog.set_secondary_text("Please set the titles directory in Settings before downloading.");
        dialog.run();
        return;
    }
    
    // Vérifier qu'il y a des jeux chargés
    if (m_cached_games.empty()) {
        Gtk::MessageDialog dialog(*this, "No Games Loaded", false, Gtk::MESSAGE_WARNING);
        dialog.set_secondary_text("Please load or scan games before downloading titles.");
        dialog.run();
        return;
    }
    
    // Vérifier si un téléchargement est déjà en cours
    if (m_thumbnail_downloader.is_downloading()) {
        Gtk::MessageDialog dialog(*this, "Download In Progress", false, Gtk::MESSAGE_INFO);
        dialog.set_secondary_text("Titles download is already in progress.");
        dialog.run();
        return;
    }
    
    // Demander confirmation à l'utilisateur
    Gtk::MessageDialog confirm_dialog(*this, "Download Titles", false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
    confirm_dialog.set_secondary_text(
        "This will download titles for " + std::to_string(m_cached_games.size()) + 
        " games from FBNeo-extras.\n\nThis may take several minutes. Continue?"
    );
    
    if (confirm_dialog.run() != Gtk::RESPONSE_YES) {
        return;
    }
    
    // Close settings dialog if it's open
    m_close_settings_signal.emit();
    
    std::cout << "[INFO] Starting titles download for " << m_cached_games.size() << " games" << std::endl;
    
    // Créer le callback de progression
    auto progress_callback = [this](const std::string& filename, int current, int total, double percentage) {
        std::cout << "[DEBUG] Progress callback called: " << filename << " - " << percentage << "%" << std::endl;
        
        // Mettre à jour les variables partagées
        m_current_download_file = filename;
        m_current_download_index = current;
        m_total_download_count = total;
        m_download_percentage = percentage;
        
        // Déclencher le dispatcher approprié
        if (percentage >= 100.0) {
            m_download_finished_dispatcher.emit();
        } else {
            m_download_progress_dispatcher.emit();
        }
    };
    
    // Démarrer le téléchargement
    m_thumbnail_downloader.start_download(m_cached_games, titles_dir, ThumbnailDownloader::ArtworkType::Titles, progress_callback);
}

void MainWindow::on_download_cancel_clicked() {
    std::cout << "[INFO] User cancelled download" << std::endl;
    
    // Cancel the download
    m_thumbnail_downloader.cancel_download();
    
    // Hide download progress and update status
    hide_download_progress();
    m_status_label.set_text("Download cancelled by user");
}

void MainWindow::start_scan_thread(const std::vector<std::string>& roms_paths) {
    if (m_scan_in_progress) return; // prevent double-launch

    m_scan_in_progress = true;
    m_scan_cancelled   = false;
    m_button_scan.set_sensitive(false);

    // Show inline scan progress in the status bar immediately.
    // show_all() is needed because set_no_show_all(true) was set at construction
    // (to prevent the box from appearing during the initial window show_all call).
    m_scan_progress_label.set_text("🔍 Initializing scan…");
    m_scan_progress_bar.set_fraction(0.0);
    // IMPORTANT: show_all() is a no-op when no_show_all=true is set on the widget.
    // We must call show() on each child and the container individually.
    m_scan_progress_label.show();
    m_scan_progress_bar.show();
    m_scan_details_button.show();
    m_scan_status_box.show();

    std::cout << "[INFO] Starting background ROM scan in "
              << roms_paths.size() << " directories..." << std::endl;

    m_database->startCacheCleanupThread(m_settings_panel.get_roms_paths());

    // Create dialog on the heap (non-modal) — destroyed when user closes it
    m_scan_dialog = std::make_unique<ROMScanDialog>(
        *this, m_database, roms_paths,
        m_settings_panel.is_scan_recursive(),
        m_settings_panel.is_scan_loose_files());

    // Notify MainWindow when scan work is done (fires on GTK main thread)
    m_scan_dialog->signal_scan_complete().connect(
        sigc::mem_fun(*this, &MainWindow::on_scan_dialog_complete));

    // "Run in Background" button → start status-bar polling
    m_scan_dialog->signal_run_in_background().connect(
        sigc::mem_fun(*this, &MainWindow::on_scan_go_background));

    // Clean up only when the dialog is hidden AND the scan is already done.
    // If the scan is still running and the user hides the dialog ("Run in Background"),
    // we keep the dialog alive so polling can continue and "Details" can reopen it.
    m_scan_dialog->signal_hide().connect([this]() {
        if (m_scan_dialog && m_scan_dialog->is_scan_finished()) {
            m_scan_bg_poll_timer.disconnect();
            m_database->stopCacheCleanupThread();
            m_scan_dialog.reset();
        }
        // Scan still running → dialog is just hidden; timer & dialog stay alive
    });

    m_scan_dialog->start_scan();
    m_scan_dialog->show();    // non-modal: returns immediately

    // Start polling immediately so the status bar shows progress from the start
    m_scan_bg_poll_timer = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &MainWindow::on_scan_bg_poll), 300);
}

void MainWindow::on_scan_dialog_complete() {
    std::cout << "[INFO] Scan complete — refreshing game list" << std::endl;

    m_scan_bg_poll_timer.disconnect();

    m_cached_games = m_database->getAllGames();

    // Regenerate the filter cache so the left panel shows updated counts/systems
    m_filter_cache = FilterCache::generate_from_games(m_cached_games);
    save_filter_cache();
    m_filter_cache_loaded = true;
    populate_filter_tree();

    filter_games();
    update_status_bar_stats();

    m_scan_in_progress = false;
    m_button_scan.set_sensitive(true);

    // Replace progress bar with a brief "done" (or "cancelled") message then hide after 4 s
    size_t avail = m_database->getGameCountByStatus("available");
    bool was_cancelled = m_scan_dialog && m_scan_dialog->was_cancelled();
    if (was_cancelled) {
        m_scan_progress_label.set_text("🛑 Scan cancelled — " + std::to_string(avail) + " games available");
    } else {
        m_scan_progress_label.set_text("✅ Scan complete — " + std::to_string(avail) + " games available");
    }
    m_scan_progress_bar.set_fraction(was_cancelled ? 0.0 : 1.0);
    m_scan_details_button.hide(); // no dialog to reopen anymore

    Glib::signal_timeout().connect_once([this]() {
        m_scan_status_box.hide();
        // Reset dialog pointer if still around (user never clicked Close)
        if (m_scan_dialog) {
            m_database->stopCacheCleanupThread();
            m_scan_dialog.reset();
        }
    }, 4000);

    std::cout << "[INFO] Interface updated after scan" << std::endl;
}

void MainWindow::on_scan_go_background() {
    // Dialog hid itself — polling was already running, nothing extra needed.
    // "📊 Details" button remains visible and reopens the dialog on click.
}

bool MainWindow::on_scan_bg_poll() {
    if (!m_scan_dialog) return false; // dialog destroyed — stop timer

    double pct = m_scan_dialog->get_scan_progress();
    std::string msg = m_scan_dialog->get_scan_message();

    // Shorten message for status bar (strip leading emoji + long paths)
    if (msg.size() > 45) msg = msg.substr(0, 42) + "…";

    int p = static_cast<int>(pct);
    m_scan_progress_label.set_text("🔍 " + std::to_string(p) + "%  " + msg);
    m_scan_progress_bar.set_fraction(pct / 100.0);

    return !m_scan_dialog->is_scan_finished(); // false stops the timer
}

void MainWindow::on_scan_progress() {
    // Update progress from scan thread via dispatcher
    int current = m_scan_current.load();
    int total = m_scan_total.load();
    
    if (total > 0) {
        double percentage = (double)current / total * 100.0;
        std::string status_text = "Scanning ROMs... " + std::to_string(current) + "/" + std::to_string(total) + " (" + std::to_string((int)percentage) + "%)";
        m_status_label.set_text(status_text);
        std::cout << status_text << std::endl;
    }
}

void MainWindow::on_scan_finished() {
    std::cout << "[INFO] ROM scan completed" << std::endl;
    
    // Reset scan state
    m_scan_in_progress = false;
    m_button_scan.set_sensitive(true);
    
    // Reload games from database and update display
    m_cached_games = m_database->getAllGames();
    filter_games();
    update_status_bar_stats();
    m_status_label.hide();
    
    // Join the worker thread
    if (m_scan_thread.joinable()) {
        m_scan_thread.join();
    }
    
    std::cout << "[INFO] Scan completed successfully - UI updated" << std::endl;
}

void MainWindow::load_filter_cache() {
    if (m_filter_cache_loaded) return;
    
    std::string cache_file = AppContext::get_user_config_dir() + "/filter_cache.json";
    
    if (FilterCache::load_from_file(cache_file, m_filter_cache)) {
        std::cout << "[INFO] Filter cache loaded successfully" << std::endl;
    } else {
        std::cout << "[WARNING] Filter cache not found. Use 'Update DAT' to generate filter cache." << std::endl;
        // Initialize with empty data - user needs to update DAT to populate filters
        m_filter_cache.systems.clear();
        m_filter_cache.manufacturers.clear();
        m_filter_cache.years.clear();
        m_filter_cache.sources.clear();
    }
    
    m_filter_cache_loaded = true;
}

void MainWindow::save_filter_cache() {
    std::string cache_file = AppContext::get_user_config_dir() + "/filter_cache.json";
    FilterCache::save_to_file(cache_file, m_filter_cache);
}

// Removed all ComboBox population methods - will implement MAMEUI-style filtering

void MainWindow::populate_filter_tree() {
    m_model_filters->clear();

    if (!m_filter_cache_loaded || m_cached_games.empty()) {
        return;
    }

    std::cout << "[INFO] Populating MAMEUI-style filter tree..." << std::endl;

    // Single pass over m_cached_games: precompute every per-category count.
    // The old code did 5+ separate passes over 25k games on the main thread,
    // which froze the UI long enough to trigger "Not Responding".
    std::unordered_map<std::string, int> system_counts;
    std::unordered_map<std::string, int> manuf_counts;
    std::unordered_map<std::string, int> year_counts;
    std::unordered_map<std::string, int> source_counts;
    std::unordered_map<std::string, int> aspect_counts;
    std::unordered_map<std::string, int> orientation_counts;
    std::unordered_map<std::string, int> status_counts;

    for (const auto& game : m_cached_games) {
        if (!game.system.empty())       system_counts[game.system]++;
        if (!game.manufacturer.empty()) manuf_counts[game.manufacturer]++;
        if (!game.year.empty())         year_counts[game.year]++;
        if (!game.sourcefile.empty()) {
            std::string game_source = game.sourcefile;
            size_t slash_pos = game_source.find('/');
            if (slash_pos != std::string::npos) game_source = game_source.substr(0, slash_pos);
            source_counts[game_source]++;
        }
        if (!game.aspect_x.empty() && !game.aspect_y.empty())
            aspect_counts[game.aspect_x + ":" + game.aspect_y]++;
        if (!game.orientation.empty()) orientation_counts[game.orientation]++;
        status_counts[game.status]++;
    }

    // Add "All Games" root item
    auto root = m_model_filters->append();
    (*root)[m_filter_columns.m_col_icon] = get_filter_icon("All Games");
    (*root)[m_filter_columns.m_col_name] = "All Games (" + std::to_string(m_cached_games.size()) + ")";
    (*root)[m_filter_columns.m_col_type] = "root";
    (*root)[m_filter_columns.m_col_value] = "All";
    (*root)[m_filter_columns.m_col_count] = m_cached_games.size();

    // Systems
    if (!m_filter_cache.systems.empty()) {
        auto systems_root = m_model_filters->append();
        (*systems_root)[m_filter_columns.m_col_icon] = get_filter_icon("Systems");
        (*systems_root)[m_filter_columns.m_col_name] = "Systems";
        (*systems_root)[m_filter_columns.m_col_type] = "category";
        (*systems_root)[m_filter_columns.m_col_value] = "";

        for (const auto& system : m_filter_cache.systems) {
            int count = system_counts[system];
            auto child = m_model_filters->append(systems_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = system + " (" + std::to_string(count) + ")";
            (*child)[m_filter_columns.m_col_type] = "system";
            (*child)[m_filter_columns.m_col_value] = system;
            (*child)[m_filter_columns.m_col_count] = count;
        }
    }

    // Manufacturers (top 20)
    if (!m_filter_cache.manufacturers.empty()) {
        auto manuf_root = m_model_filters->append();
        (*manuf_root)[m_filter_columns.m_col_icon] = get_filter_icon("Manufacturers");
        (*manuf_root)[m_filter_columns.m_col_name] = "Manufacturers";
        (*manuf_root)[m_filter_columns.m_col_type] = "category";
        (*manuf_root)[m_filter_columns.m_col_value] = "";

        std::vector<std::pair<std::string, int>> sorted_manufs(manuf_counts.begin(), manuf_counts.end());
        std::sort(sorted_manufs.begin(), sorted_manufs.end(),
                 [](const auto& a, const auto& b) { return a.second > b.second; });

        int added = 0;
        for (const auto& [manuf, count] : sorted_manufs) {
            if (added >= 20) break;
            auto child = m_model_filters->append(manuf_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = manuf + " (" + std::to_string(count) + ")";
            (*child)[m_filter_columns.m_col_type] = "manufacturer";
            (*child)[m_filter_columns.m_col_value] = manuf;
            (*child)[m_filter_columns.m_col_count] = count;
            added++;
        }
    }

    // Years (grouped by decade)
    if (!m_filter_cache.years.empty()) {
        auto years_root = m_model_filters->append();
        (*years_root)[m_filter_columns.m_col_icon] = get_filter_icon("Years");
        (*years_root)[m_filter_columns.m_col_name] = "Years";
        (*years_root)[m_filter_columns.m_col_type] = "category";
        (*years_root)[m_filter_columns.m_col_value] = "";

        std::map<std::string, std::vector<std::string>> decades;
        for (const auto& year : m_filter_cache.years) {
            if (year.length() == 4) {
                std::string decade = year.substr(0, 3) + "0s";
                decades[decade].push_back(year);
            }
        }

        for (const auto& [decade, years] : decades) {
            auto decade_node = m_model_filters->append(years_root->children());
            (*decade_node)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*decade_node)[m_filter_columns.m_col_name] = decade;
            (*decade_node)[m_filter_columns.m_col_type] = "category";
            (*decade_node)[m_filter_columns.m_col_value] = "";

            for (const auto& year : years) {
                int count = year_counts[year];
                auto child = m_model_filters->append(decade_node->children());
                (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
                (*child)[m_filter_columns.m_col_name] = year + " (" + std::to_string(count) + ")";
                (*child)[m_filter_columns.m_col_type] = "year";
                (*child)[m_filter_columns.m_col_value] = year;
                (*child)[m_filter_columns.m_col_count] = count;
            }
        }
    }

    // Sources
    if (!m_filter_cache.sources.empty()) {
        auto sources_root = m_model_filters->append();
        (*sources_root)[m_filter_columns.m_col_icon] = get_filter_icon("Sources");
        (*sources_root)[m_filter_columns.m_col_name] = "Sources";
        (*sources_root)[m_filter_columns.m_col_type] = "category";
        (*sources_root)[m_filter_columns.m_col_value] = "";

        for (const auto& source : m_filter_cache.sources) {
            int count = source_counts[source];
            auto child = m_model_filters->append(sources_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = source + " (" + std::to_string(count) + ")";
            (*child)[m_filter_columns.m_col_type] = "source";
            (*child)[m_filter_columns.m_col_value] = source;
            (*child)[m_filter_columns.m_col_count] = count;
        }
    }

    // Aspect Ratio
    auto aspect_root = m_model_filters->append();
    (*aspect_root)[m_filter_columns.m_col_icon] = get_filter_icon("Aspect Ratio");
    (*aspect_root)[m_filter_columns.m_col_name] = "Aspect Ratio";
    (*aspect_root)[m_filter_columns.m_col_type] = "category";
    (*aspect_root)[m_filter_columns.m_col_value] = "";

    for (const auto& [aspect, count] : aspect_counts) {
        auto child = m_model_filters->append(aspect_root->children());
        (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
        (*child)[m_filter_columns.m_col_name] = aspect + " (" + std::to_string(count) + ")";
        (*child)[m_filter_columns.m_col_type] = "aspect";
        (*child)[m_filter_columns.m_col_value] = aspect;
        (*child)[m_filter_columns.m_col_count] = count;
    }

    // Orientation
    auto orientation_root = m_model_filters->append();
    (*orientation_root)[m_filter_columns.m_col_icon] = get_filter_icon("Orientation");
    (*orientation_root)[m_filter_columns.m_col_name] = "Orientation";
    (*orientation_root)[m_filter_columns.m_col_type] = "category";
    (*orientation_root)[m_filter_columns.m_col_value] = "";

    for (const auto& [orientation, count] : orientation_counts) {
        auto child = m_model_filters->append(orientation_root->children());
        (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
        (*child)[m_filter_columns.m_col_name] = orientation + " (" + std::to_string(count) + ")";
        (*child)[m_filter_columns.m_col_type] = "orientation";
        (*child)[m_filter_columns.m_col_value] = orientation;
        (*child)[m_filter_columns.m_col_count] = count;
    }

    // ROM Status
    auto status_root = m_model_filters->append();
    (*status_root)[m_filter_columns.m_col_icon] = get_filter_icon("ROM Status");
    (*status_root)[m_filter_columns.m_col_name] = "ROM Status";
    (*status_root)[m_filter_columns.m_col_type] = "category";
    (*status_root)[m_filter_columns.m_col_value] = "";

    for (const auto& [status, count] : status_counts) {
        auto child = m_model_filters->append(status_root->children());
        (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
        (*child)[m_filter_columns.m_col_name] = status + " (" + std::to_string(count) + ")";
        (*child)[m_filter_columns.m_col_type] = "status";
        (*child)[m_filter_columns.m_col_value] = status;
        (*child)[m_filter_columns.m_col_count] = count;
    }

    m_treeview_filters.collapse_all();
    std::cout << "[INFO] Filter tree populated successfully" << std::endl;
}

void MainWindow::on_filter_selection_changed() {
    auto selection = m_treeview_filters.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;
    
    Gtk::TreeModel::Row row = *iter;
    Glib::ustring type_ustring = row[m_filter_columns.m_col_type];
    Glib::ustring value_ustring = row[m_filter_columns.m_col_value];
    std::string type = type_ustring.raw();
    std::string value = value_ustring.raw();
    
    if (type == "category" || type == "root") {
        // Clear all filters for root/category selections
        m_active_filters.clear();
    } else {
        // Set filter for specific type
        m_active_filters[type] = value;
    }
    
    apply_tree_filters();
}

void MainWindow::apply_tree_filters() {
    // Detach the model and disable sort during the bulk rebuild — GTK
    // otherwise refreshes the view (and re-sorts) on every append, which
    // freezes the UI long enough to trigger "Not Responding" on 25k+ rows.
    m_treeview_games.unset_model();
    int prev_sort_col = Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID;
    Gtk::SortType prev_sort_order = Gtk::SORT_ASCENDING;
    bool had_sort = m_model_games->get_sort_column_id(prev_sort_col, prev_sort_order);
    m_model_games->set_sort_column(Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID, Gtk::SORT_ASCENDING);
    m_model_games->clear();

    std::string search_text = m_search_entry.get_text();
    std::transform(search_text.begin(), search_text.end(), search_text.begin(), ::tolower);
    
    std::vector<Game> filtered_games;
    
    for (const auto& game : m_cached_games) {
        bool matches = true;
        
        // Apply active filters
        for (const auto& [filter_type, filter_value] : m_active_filters) {
            if (filter_type == "system" && game.system != filter_value) {
                matches = false; break;
            }
            if (filter_type == "manufacturer" && game.manufacturer != filter_value) {
                matches = false; break;
            }
            if (filter_type == "year" && game.year != filter_value) {
                matches = false; break;
            }
            if (filter_type == "source") {
                std::string game_source = game.sourcefile;
                size_t slash_pos = game_source.find('/');
                if (slash_pos != std::string::npos) {
                    game_source = game_source.substr(0, slash_pos);
                }
                if (game_source != filter_value) {
                    matches = false; break;
                }
            }
            if (filter_type == "aspect") {
                std::string game_aspect = "";
                if (!game.aspect_x.empty() && !game.aspect_y.empty()) {
                    game_aspect = game.aspect_x + ":" + game.aspect_y;
                }
                if (game_aspect != filter_value) {
                    matches = false; break;
                }
            }
            if (filter_type == "orientation" && game.orientation != filter_value) {
                matches = false; break;
            }
            if (filter_type == "status" && game.status != filter_value) {
                matches = false; break;
            }
            if (filter_type == "mode" && filter_value == "console") {
                // Console mode - exclude arcade games
                if (game.system == "Arcade") {
                    matches = false; break;
                }
            }
        }
        
        if (!matches) continue;
        
        // Apply search filter
        if (!search_text.empty()) {
            std::string game_name  = game.name;
            std::string game_desc  = game.description;
            std::string game_manuf = game.manufacturer;
            std::string game_year  = game.year;
            std::transform(game_name.begin(),  game_name.end(),  game_name.begin(),  ::tolower);
            std::transform(game_desc.begin(),  game_desc.end(),  game_desc.begin(),  ::tolower);
            std::transform(game_manuf.begin(), game_manuf.end(), game_manuf.begin(), ::tolower);
            // year is numeric — compare as-is (search_text already lowered, no-op for digits)

            if (game_name.find(search_text)  == std::string::npos &&
                game_desc.find(search_text)  == std::string::npos &&
                game_manuf.find(search_text) == std::string::npos &&
                game_year.find(search_text)  == std::string::npos) {
                continue;
            }
        }
        
        filtered_games.push_back(game);
    }
    
    // Update TreeView
    for (const auto& game : filtered_games) {
        auto row = *m_model_games->append();
        row[m_columns.m_col_icon]     = IconManager::get_status_icon(game.status);
        row[m_columns.m_col_status]   = game.status;   // needed by the detail dock pills
        row[m_columns.m_col_favorite] = game.is_favorite;
        row[m_columns.m_col_name]     = game.name;
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
    
    // Restore sort + reattach model (single redraw instead of one per insert)
    if (had_sort &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID) {
        m_model_games->set_sort_column(prev_sort_col, prev_sort_order);
    }
    m_treeview_games.set_model(m_model_games);

    // Update stats
    {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        m_filtered_games = filtered_games;
    }

    update_status_bar_stats();
}

void MainWindow::update_filter_counts() {
    // This can be called to refresh counts without rebuilding the whole tree
    // For now, we'll just repopulate the tree
    populate_filter_tree();
}

Glib::RefPtr<Gdk::Pixbuf> MainWindow::get_filter_icon(const std::string& category) {
    // Cache pixbufs per category — called once per filter row and the "item"
    // bucket is hit dozens of times per filter-tree rebuild.
    static std::unordered_map<std::string, Glib::RefPtr<Gdk::Pixbuf>> cache;
    auto it = cache.find(category);
    if (it != cache.end()) return it->second;

    std::string icon_file;
    if (category == "All Games")          icon_file = "filter-folder.svg";
    else if (category == "Systems")       icon_file = "filter-systems.svg";
    else if (category == "Manufacturers") icon_file = "filter-manufacturers.svg";
    else if (category == "Years")         icon_file = "filter-years.svg";
    else if (category == "Sources")       icon_file = "filter-sources.svg";
    else if (category == "Aspect Ratio")  icon_file = "filter-aspect.svg";
    else if (category == "Orientation")   icon_file = "filter-orientation.svg";
    else if (category == "ROM Status")    icon_file = "filter-status.svg";
    else                                  icon_file = "filter-item.svg";

    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    try {
        std::string icon_path = AppContext::get_executable_dir() + "/assets/icons/" + icon_file;
        // Icons authored with `currentColor` rasterize to black when loaded
        // standalone — invisible on the dark theme. Recolor to a light tint first.
        std::ifstream f(icon_path);
        std::string svg((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (svg.find("currentColor") != std::string::npos) {
            const std::string from = "currentColor", to = "#c7cbd6";
            for (size_t p = svg.find(from); p != std::string::npos; p = svg.find(from, p + to.size()))
                svg.replace(p, from.size(), to);
            auto loader = Gdk::PixbufLoader::create("svg");
            loader->set_size(20, 20);
            loader->write(reinterpret_cast<const guint8*>(svg.data()), svg.size());
            loader->close();
            pixbuf = loader->get_pixbuf();
        }
        if (!pixbuf)
            pixbuf = Gdk::Pixbuf::create_from_file(icon_path, 20, 20);
    } catch (const std::exception&) {
        pixbuf = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 16, 16);
    }

    cache.emplace(category, pixbuf);
    return pixbuf;
}

// ── Launch preference persistence ─────────────────────────────────────────

void MainWindow::populate_language_combo() {
    // Flag + native name for each shipped language; only those with a catalog
    // (plus English) are shown.
    static const std::vector<std::pair<std::string, std::string>> langs = {
        {"en", "🇬🇧 English"}, {"fr", "🇫🇷 Français"}, {"es", "🇪🇸 Español"},
        {"de", "🇩🇪 Deutsch"}, {"pt", "🇵🇹 Português"}, {"zh", "🇨🇳 中文"}, {"ja", "🇯🇵 日本語"}};
    auto avail = i18n::available_languages();
    for (const auto& [code, label] : langs)
        if (std::find(avail.begin(), avail.end(), code) != avail.end())
            m_lang_combo.append(code, label);

    // Reflect the currently active language without triggering the change handler.
    m_suppress_lang_signal = true;
    if (!m_lang_combo.set_active_id(i18n::language()))
        m_lang_combo.set_active_id("en");
    m_suppress_lang_signal = false;
}

void MainWindow::on_language_selected(const std::string& code) {
    // Persist the choice, keep both language controls in sync, and prompt for a
    // restart (labels are built once at startup).
    m_settings_panel.set_language(code); // suppressed inside SettingsPanel
    m_suppress_lang_signal = true;
    m_lang_combo.set_active_id(code.empty() ? std::string("en") : code);
    m_suppress_lang_signal = false;
    m_settings_panel.save_to_file(AppContext::get_config_path());

    Gtk::MessageDialog dlg(*this, _("Language changed"), false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(_("Restart the launcher to fully apply the new language."));
    dlg.run();
}

void MainWindow::apply_theme(const std::string& mode) {
    auto screen = Gdk::Screen::get_default();
    if (!screen) return;

    // Lazily create the providers on first use.
    if (!m_css_common) {
        m_css_common = Gtk::CssProvider::create();
        try { m_css_common->load_from_path(AppContext::get_asset_path("style-common.css")); }
        catch (const Glib::Error& e) { std::cerr << "[WARN] style-common.css: " << e.what() << std::endl; }
        Gtk::StyleContext::add_provider_for_screen(screen, m_css_common, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    if (!m_css_dark) {
        m_css_dark = Gtk::CssProvider::create();
        try { m_css_dark->load_from_path(AppContext::get_asset_path("style-dark.css")); }
        catch (const Glib::Error& e) { std::cerr << "[WARN] style-dark.css: " << e.what() << std::endl; }
    }

    // Prefer-dark on the base theme. "system" leaves the base theme untouched.
    if (auto settings = Gtk::Settings::get_default()) {
        if (mode == "dark")       settings->property_gtk_application_prefer_dark_theme() = true;
        else if (mode == "light") settings->property_gtk_application_prefer_dark_theme() = false;
    }

    // The dark surface overrides apply only in Dark mode.
    Gtk::StyleContext::remove_provider_for_screen(screen, m_css_dark);
    if (mode == "dark")
        Gtk::StyleContext::add_provider_for_screen(screen, m_css_dark, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    m_theme_mode = mode;
}

void MainWindow::load_launch_prefs() {
    std::ifstream fi(AppContext::get_config_path());
    if (!fi) return;
    try {
        nlohmann::json j;
        fi >> j;
        if (j.contains("launch_fullscreen"))   m_launch_fullscreen   = j["launch_fullscreen"].get<bool>();
        if (j.contains("launch_integerscale")) m_launch_integerscale = j["launch_integerscale"].get<bool>();
    } catch (...) {}
    // Reflect loaded state in menu checkitems (block toggled signal to avoid side-effect)
    m_menu_item_fullscreen_mode.set_active(m_launch_fullscreen);
    m_menu_item_integerscale_mode.set_active(m_launch_integerscale);
}

void MainWindow::save_launch_prefs() {
    const std::string cfg = AppContext::get_config_path();
    nlohmann::json j;
    {
        std::ifstream fi(cfg);
        if (fi) { try { fi >> j; } catch (...) {} }
    }
    j["launch_fullscreen"]   = m_launch_fullscreen;
    j["launch_integerscale"] = m_launch_integerscale;
    std::ofstream fo(cfg);
    if (fo) fo << j.dump(4) << std::endl;
}

// ── ZIP path lookup ────────────────────────────────────────────────────────

std::string MainWindow::find_rom_zip_path(const std::string& rom_name) {
    const std::string zip_name = rom_name + ".zip";
    for (const auto& dir : m_settings_panel.get_roms_paths()) {
        try {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (entry.path().filename() == zip_name)
                    return entry.path().string();
            }
        } catch (...) {}
    }
    return "";
}

// ── ZIP integrity check ────────────────────────────────────────────────────

bool MainWindow::verify_zip_integrity(const std::string& zip_path) {
    int err = 0;
    zip_t* z = zip_open(zip_path.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &err);
    if (!z) return false;
    zip_close(z);
    return true;
}

// ── Duplicate ROM detection ────────────────────────────────────────────────

void MainWindow::on_find_duplicate_roms() {
    auto rom_paths = m_settings_panel.get_roms_paths();
    if (rom_paths.empty()) {
        Gtk::MessageDialog dlg(*this, "No ROM directories configured", false, Gtk::MESSAGE_INFO);
        dlg.run();
        return;
    }

    // Collect zip files: name (stem) → list of full paths
    std::map<std::string, std::vector<std::string>> seen;
    for (const auto& dir : rom_paths) {
        try {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (entry.path().extension() == ".zip")
                    seen[entry.path().stem().string()].push_back(entry.path().string());
            }
        } catch (...) {}
    }

    // Filter to actual duplicates
    std::vector<std::pair<std::string, std::vector<std::string>>> dupes;
    for (auto& [name, paths] : seen)
        if (paths.size() > 1) dupes.push_back({name, paths});

    if (dupes.empty()) {
        Gtk::MessageDialog dlg(*this, "No duplicates found", false, Gtk::MESSAGE_INFO);
        dlg.set_secondary_text("No duplicate ROM ZIP files were found across the configured directories.");
        dlg.run();
        return;
    }

    // Sort by name
    std::sort(dupes.begin(), dupes.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    std::ostringstream oss;
    oss << dupes.size() << " duplicate ROM(s) found:\n\n";
    for (const auto& [name, paths] : dupes) {
        oss << name << ".zip (" << paths.size() << " copies):\n";
        for (const auto& p : paths)
            oss << "  " << p << "\n";
        oss << "\n";
    }

    // Show in a scrollable dialog
    Gtk::Dialog dlg("Duplicate ROMs", *this, Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(700, 420);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* sw = Gtk::make_managed<Gtk::ScrolledWindow>();
    sw->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw->set_margin_start(12); sw->set_margin_end(12);
    sw->set_margin_top(10);  sw->set_margin_bottom(10);
    sw->set_vexpand(true);

    auto* tv = Gtk::make_managed<Gtk::TextView>();
    tv->set_editable(false);
    tv->set_monospace(true);
    tv->get_buffer()->set_text(oss.str());
    sw->add(*tv);

    dlg.get_content_area()->pack_start(*sw, Gtk::PACK_EXPAND_WIDGET);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    dlg.show_all_children();
    dlg.run();
}
