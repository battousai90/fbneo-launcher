// src/MainWindow.cpp
#include "MainWindow.h"
#include "i18n.h"
#include <iostream>
#include "DatParser.h"
#include "SettingsPanel.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include "FbneoUpdateCheck.h"
#include "ScreenshotAssignDialog.h"
#include "SystemPrefix.h"
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <zip.h>
#include <sstream>
#include <ctime>

// Launch an external process without invoking a shell.
// args[0] must be the executable path; remaining entries are its arguments.
// Returns the child PID on success, -1 on failure.
//
// FinalBurn Neo lives on the host and is linked against the host's libraries
// (SDL2 and friends), none of which exist inside our sandbox : running it
// directly from a Flatpak fails with "error while loading shared libraries".
// So when sandboxed, the command is handed to flatpak-spawn, which executes it
// on the host. That is what the --talk-name=org.freedesktop.Flatpak permission
// in the manifest is for.
// std::filesystem::last_write_time()'s usual duration_cast-to-seconds idiom
// does NOT yield a Unix timestamp on this toolchain under strict C++17 // std::filesystem::file_clock's epoch here is not 1970 (confirmed empirically:
// off by billions of seconds), and file_clock::to_sys() needs C++20. stat()
// gives a real time_t directly, no epoch ambiguity.
static std::time_t get_file_mtime(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return st.st_mtime;
}

static pid_t spawn_process(const std::vector<std::string>& args) {
    if (args.empty()) return -1;

    const std::vector<std::string> cmd = AppContext::host_command(args);

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[ERROR] fork() failed for: " << args[0] << std::endl;
        return -1;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(cmd.size() + 1);
        for (const auto& a : cmd)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::cerr << "[ERROR] execvp failed for: " << cmd[0] << std::endl;
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

// Where FBNeo keeps the raw RAM dump it writes when a game with hiscore
// support exits. Named after the FBNeo ROM name, which carries the console
// prefix : not after the catalogue name used to identify the game online.
static std::string fbneo_hiscore_path(const std::string& fbneo_rom_name) {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") +
           "/.local/share/fbneo/support/hiscores/" + fbneo_rom_name + ".hi";
}

// Returns "" when the file is absent, which is the normal state before a
// game has ever been played to a score worth keeping.
static std::string read_file_bytes(const std::string& path) {
    std::ifstream fi(path, std::ios::binary);
    if (!fi) return {};
    return std::string(std::istreambuf_iterator<char>(fi),
                       std::istreambuf_iterator<char>());
}

// "FR" -> the French flag. Built from Unicode regional indicators, which is
// what makes the fallback graceful: a system with no flag glyph draws the two
// letters instead of a blank or a placeholder box.
static std::string country_flag(const std::string& iso) {
    if (iso.size() != 2) return {};
    std::string out;
    for (char c : iso) {
        if (c < 'A' || c > 'Z') return {};       // lower-case or junk: skip it
        gunichar cp = 0x1F1E6 + (c - 'A');
        char buf[8] = {0};
        out.append(buf, g_unichar_to_utf8(cp, buf));
    }
    return out;
}

// "2026-08-29T09:11:57Z" -> "2026-08-29". The time of day says nothing a
// player wants; the day is what places a record in the life of a leaderboard.
static std::string short_date(const std::string& iso8601) {
    return iso8601.size() >= 10 ? iso8601.substr(0, 10) : std::string();
}

// "2 h 14", "37 min", "45 s" : the coarsest unit that still says something.
// A session is read at a glance, so seconds past the first minute are noise.
static std::string format_duration(int seconds) {
    if (seconds <= 0) return "";
    if (seconds < 60)   return std::to_string(seconds) + " s";
    int minutes = seconds / 60;
    if (minutes < 60)   return std::to_string(minutes) + " min";
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60);
}

// Thin-space grouping: 1234567 -> "1 234 567". Scores are the one figure a
// player compares to someone else's, and ungrouped digits make that slow.
static std::string format_score(long long value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += "\u202f";
        out += digits[i];
    }
    return (value < 0 ? "-" : "") + out;
}

MainWindow::MainWindow(std::shared_ptr<DatabaseManager> database,
                       std::function<void(double, const std::string&)> progress_callback,
                       const std::vector<Game>& preloaded_games) {
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    m_button_scan.set_label(_("ROM Manager"));
    m_download_cancel_button.set_label(_("Cancel"));

    std::cout << "[DEBUG] MainWindow constructor started" << std::endl;

    if (progress_callback) progress_callback(0.75, "Setting up interface...");
    set_title("FBNeo Launcher");
    set_default_size(1400, 800);  // Larger default size for better column display
    set_border_width(8);
    try { set_icon(Gdk::Pixbuf::create_from_file(AppContext::get_asset_path("logo.svg"), 64, 64)); } catch (...) {}

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
    // Reuse the connection opened in main() : opening a second sqlite3 handle on
    // the same file caused write contention and double-init noise in the log.
    m_database = database;
    if (!m_database) {
        std::cerr << "[ERROR] No database handle passed to MainWindow" << std::endl;
        m_status_label.set_text(_("Error: Failed to initialize database"));
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
    // Effet immediat : un interrupteur qui exige un redemarrage n'est pas un
    // interrupteur. Eteint, on vide la liste et on repeint pour que les
    // pastilles disparaissent tout de suite ; allume, on recharge.
    m_settings_panel.signal_hiscore_toggled().connect([this](bool on) {
        if (on) {
            refresh_hiscore_data_async(true);
        } else {
            {
                std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
                m_hiscore_supported.clear();
            }
            m_hiscore_box.hide();
            on_hiscore_supported_ready();
            m_status_label.set_text(_("Online highscores turned off."));
        }
    });

    m_settings_panel.signal_language_changed().connect([this](Glib::ustring code) {
        on_language_selected(code);
    });

    // === Menu Bar ===
    // File Menu
    m_menu_file.set_label(_("File"));
    m_menu_file.set_submenu(m_submenu_file);
    m_app_menu.append(m_menu_file);
    
    m_menu_item_settings.set_label(_("Launcher Settings..."));
    m_menu_item_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_settings_clicked));
    m_submenu_file.append(m_menu_item_settings);
    
    m_menu_item_export_game_list.set_label(_("Export Game List..."));
    m_menu_item_export_game_list.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_export_game_list));
    m_submenu_file.append(m_menu_item_export_game_list);

    // Rafraîchissement à la demande. Les classements se mettent à jour seuls
    // au démarrage et toutes les quinze minutes ; ceci est pour le joueur qui
    // vient de battre un ami et ne veut pas attendre le prochain cycle. Il
    // demande, donc il accepte l'attente : mais la barre d'état doit le lui
    // dire, sinon il recommence en croyant qu'il ne s'est rien passé.
    m_menu_item_refresh_hiscores.set_label(_("Refresh highscores"));
    m_menu_item_refresh_hiscores.signal_activate().connect(
        [this]() { refresh_hiscore_data_async(true); });
    m_submenu_file.append(m_menu_item_refresh_hiscores);
    
    m_submenu_file.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    
    m_menu_item_quit.set_label(_("Quit"));
    m_menu_item_quit.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_quit));
    m_submenu_file.append(m_menu_item_quit);
    
    // Emulator Menu
    m_menu_emulator.set_label(_("Emulator"));
    m_menu_emulator.set_submenu(m_submenu_emulator);
    m_app_menu.append(m_menu_emulator);
    
    m_menu_item_fbneo_menu.set_label(_("Open FBNeo Menu"));
    m_menu_item_fbneo_menu.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_fbneo_menu));
    m_submenu_emulator.append(m_menu_item_fbneo_menu);

    m_menu_item_input_settings.set_label(_("Controller Settings..."));
    m_menu_item_input_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_input_settings));
    m_submenu_emulator.append(m_menu_item_input_settings);

    m_submenu_emulator.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    
    m_menu_item_fullscreen_mode.set_label(_("Launch in Fullscreen  (-fullscreen)"));
    m_menu_item_fullscreen_mode.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::on_fullscreen_mode));
    m_submenu_emulator.append(m_menu_item_fullscreen_mode);

    m_menu_item_integerscale_mode.set_label(_("Use Integer Scale  (-integerscale)"));
    m_menu_item_integerscale_mode.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::on_integerscale_mode));
    m_submenu_emulator.append(m_menu_item_integerscale_mode);
    
    m_submenu_emulator.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    
    m_menu_item_download_latest_fbneo.set_label(_("Download Latest FBNeo Release"));
    m_menu_item_download_latest_fbneo.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_download_latest_fbneo));
    m_submenu_emulator.append(m_menu_item_download_latest_fbneo);
    
    m_menu_item_generate_dat_files.set_label(_("Generate DAT Files"));
    m_menu_item_generate_dat_files.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_generate_dat_files));
    m_submenu_emulator.append(m_menu_item_generate_dat_files);
    
    // Filter Menu
    m_menu_filter.set_label(_("Filter"));
    m_menu_filter.set_submenu(m_submenu_filter);
    m_app_menu.append(m_menu_filter);
    
    m_menu_item_all_systems.set_label(_("Show All Games"));
    m_menu_item_all_systems.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_all_systems));
    m_submenu_filter.append(m_menu_item_all_systems);
    
    m_menu_item_arcade_mode.set_label(_("Arcade Games Only"));
    m_menu_item_arcade_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_arcade_mode));
    m_submenu_filter.append(m_menu_item_arcade_mode);
    
    m_menu_item_console_mode.set_label(_("Console Games Only"));
    m_menu_item_console_mode.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_console_mode));
    m_submenu_filter.append(m_menu_item_console_mode);
    
    m_menu_item_show_available_only.set_label(_("Available ROMs Only"));
    m_menu_item_show_available_only.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_show_available_only));
    m_submenu_filter.append(m_menu_item_show_available_only);
    
    m_menu_item_show_missing_roms.set_label(_("Missing ROMs Only"));
    m_menu_item_show_missing_roms.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_show_missing_roms));
    m_submenu_filter.append(m_menu_item_show_missing_roms);
    
    // ROMs Menu
    m_menu_roms.set_label(_("ROMs"));
    m_menu_roms.set_submenu(m_submenu_roms);
    m_app_menu.append(m_menu_roms);
    
    m_menu_item_rescan_roms.set_label(_("Rescan ROMs"));
    m_menu_item_rescan_roms.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_rescan_roms));
    m_submenu_roms.append(m_menu_item_rescan_roms);
    
    m_menu_item_update_dat.set_label(_("Update DAT"));
    m_menu_item_update_dat.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_update_dat_clicked));
    m_submenu_roms.append(m_menu_item_update_dat);

    m_menu_item_find_duplicates.set_label(_("Find Duplicate ROMs..."));
    m_menu_item_find_duplicates.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_find_duplicate_roms));
    m_submenu_roms.append(m_menu_item_find_duplicates);

    m_submenu_roms.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m_menu_item_rom_manager.set_label(_("ROM Management…"));
    m_menu_item_rom_manager.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_rom_manager));
    m_submenu_roms.append(m_menu_item_rom_manager);

    // Help Menu
    m_menu_help.set_label(_("Help"));
    m_menu_help.set_submenu(m_submenu_help);
    m_app_menu.append(m_menu_help);
    
    m_menu_item_about_fbneo.set_label(_("About FinalBurn Neo"));
    m_menu_item_about_fbneo.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_about_fbneo));
    m_submenu_help.append(m_menu_item_about_fbneo);
    
    m_menu_item_about_launcher.set_label(_("About Launcher"));
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
        m_button_scan.set_image_from_icon_name("drive-harddisk-symbolic", Gtk::ICON_SIZE_BUTTON);
    }
    m_button_scan.set_always_show_image(true);
    m_button_scan.set_tooltip_text(_("ROM Manager"));

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
    m_treeview_games.append_column("HI", m_columns.m_col_hiscore);
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
    m_label_title.set_markup("<b>Select a game to play</b>");  // This is safe static text
    m_button_play.set_sensitive(false);
    m_button_play.set_size_request(120, 32); // Force minimum size
    m_button_download_art.set_sensitive(false);
    m_button_download_art.set_size_request(140, 32); // Slightly wider for "Download Art"

    // Detail dock. Two artworks (Title on top, Preview below) sit in an image
    // column; the info column carries the big title, the full metadata block and
    // the action buttons. m_details_box reflows between horizontal (bottom dock)
    // and vertical (right dock) : see set_dock_position().
    m_details_box.get_style_context()->add_class("detail-dock");
    m_details_box.set_margin_start(12);
    m_details_box.set_margin_end(12);
    m_details_box.set_margin_top(10);
    m_details_box.set_margin_bottom(10);

    // Group A : the two artworks (Title + Preview). Orientation flips per dock.
    m_title_image.get_style_context()->add_class("dock-thumb");
    m_preview_image.get_style_context()->add_class("dock-thumb");
    // Centre : cale a gauche dans un volet large, l'artwork laissait un vide
    // a droite et donnait un panneau bancal. Les deux images partagent le meme
    // cadre pour qu'elles s'alignent au lieu d'avoir chacune sa largeur.
    m_detail_image_wrap.set_valign(Gtk::ALIGN_START);
    m_title_image.get_style_context()->add_class("dock-art");
    m_preview_image.get_style_context()->add_class("dock-art");
    m_detail_image_wrap.pack_start(m_title_image, Gtk::PACK_SHRINK);
    m_detail_image_wrap.pack_start(m_preview_image, Gtk::PACK_SHRINK);
    m_detail_image_wrap.set_hexpand(false);
    m_details_box.pack_start(m_detail_image_wrap, Gtk::PACK_SHRINK);

    // Group B : big title + full metadata block.
    m_label_title.set_xalign(0.5f);
    m_label_title.set_justify(Gtk::JUSTIFY_CENTER);
    m_label_title.set_line_wrap(true);
    m_label_title.get_style_context()->add_class("dock-title");
    // Editeur, annee, systeme sur une ligne sous le titre : les trois choses
    // qu'on veut savoir avant tout le reste, sans avoir a lire le tableau.
    m_label_meta.set_xalign(0.5f);
    m_label_meta.set_justify(Gtk::JUSTIFY_CENTER);
    m_label_meta.get_style_context()->add_class("dock-meta");
    m_label_info.set_xalign(0.0f);
    m_label_info.set_line_wrap(true);
    m_label_info.set_valign(Gtk::ALIGN_START);
    m_label_info.get_style_context()->add_class("dock-sub");

    // Deux colonnes : intitule en retrait, valeur en avant. C'est ce qui fait
    // qu'on trouve une information d'un coup d'oeil au lieu de lire une liste.
    m_specs_grid.set_row_spacing(3);
    m_specs_grid.set_column_spacing(14);

    m_detail_text_col.set_valign(Gtk::ALIGN_START);
    // Une colonne de largeur fixe, centree. Sans borne, le volet s'etirait a
    // la largeur de la fenetre : les cartes traversaient l'ecran, le titre
    // flottait au milieu d'un vide et les pastilles restaient collees a
    // gauche. Ce n'est pas un centrage qui manquait, c'est une colonne.
    m_detail_text_col.pack_start(m_label_title, Gtk::PACK_SHRINK);
    m_detail_text_col.pack_start(m_label_meta, Gtk::PACK_SHRINK);
    // Les pastilles d'etat remontent sous le titre : elles decrivent le jeu,
    // leur place n'est pas en bas a cote des boutons d'action.
    m_detail_text_col.pack_start(m_dock_pills, Gtk::PACK_SHRINK);
    m_detail_text_col.pack_start(m_label_info, Gtk::PACK_SHRINK);
    // Les caracteristiques dans une carte, comme le tableau des scores : deux
    // blocs identifiables valent mieux qu'un ruissellement de lignes.
    m_specs_grid.get_style_context()->add_class("spec-card");
    m_detail_text_col.pack_start(m_specs_grid, Gtk::PACK_SHRINK);

    m_hiscore_title.set_xalign(0.0f);
    m_hiscore_title.get_style_context()->add_class("hi-heading");
    m_hiscore_note.set_xalign(0.0f);
    m_hiscore_note.get_style_context()->add_class("hi-note");
    m_btn_hiscore_refresh.set_tooltip_text(
        _("Fetch the latest scores now, without waiting for the next automatic refresh."));
    m_btn_hiscore_refresh.get_style_context()->add_class("hi-refresh");
    m_btn_hiscore_refresh.set_relief(Gtk::RELIEF_NONE);
    m_btn_hiscore_refresh.set_image_from_icon_name("view-refresh-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_btn_hiscore_refresh.signal_clicked().connect(
        [this]() { refresh_hiscore_data_async(true); });
    m_hiscore_head.pack_start(m_hiscore_title, Gtk::PACK_SHRINK);
    m_hiscore_head.pack_end(m_btn_hiscore_refresh, Gtk::PACK_SHRINK);

    m_hiscore_grid.set_row_spacing(1);
    m_hiscore_grid.set_column_spacing(12);
    m_hiscore_grid.get_style_context()->add_class("hi-board");
    m_hiscore_grid.set_hexpand(true);

    m_label_hiscore.set_xalign(0.0f);
    m_label_hiscore.get_style_context()->add_class("hi-note");

    m_hiscore_box.pack_start(m_hiscore_head, Gtk::PACK_SHRINK);
    m_hiscore_box.pack_start(m_hiscore_grid, Gtk::PACK_SHRINK);
    m_hiscore_box.pack_start(m_label_hiscore, Gtk::PACK_SHRINK);
    m_hiscore_box.set_margin_top(14);
    m_hiscore_box.set_no_show_all(true);
    m_detail_text_col.pack_start(m_hiscore_box, Gtk::PACK_SHRINK);
    m_details_box.pack_start(m_detail_text_col, Gtk::PACK_EXPAND_WIDGET);

    // Group C : status pills above the action buttons.
    m_button_favorite.set_tooltip_text(_("Toggle favorite"));
    m_button_favorite.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_dock_favorite_clicked));
    m_detail_actions.pack_start(m_button_play, Gtk::PACK_SHRINK);
    m_detail_actions.pack_start(m_button_download_art, Gtk::PACK_SHRINK);
    m_detail_actions.pack_start(m_button_favorite, Gtk::PACK_SHRINK);
    m_detail_actions_col.set_valign(Gtk::ALIGN_START);
    m_detail_actions_col.pack_start(m_detail_actions, Gtk::PACK_SHRINK);
    m_details_box.pack_start(m_detail_actions_col, Gtk::PACK_SHRINK);

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
    // Exact column count, driven by the 3/4/5 selector (see set_grid_columns).
    m_flowbox.set_min_children_per_line(m_grid_columns);
    m_flowbox.set_max_children_per_line(m_grid_columns);
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
    // value_changed -> the user scrolled; changed -> content or viewport resized,
    // which also covers a first batch too short to fill the window.
    if (auto adj = m_scrolled_grid.get_vadjustment()) {
        adj->signal_value_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_grid));
        adj->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_grid));
    }

    // Modern list view (styled ListBox of rows).
    m_mlist.set_selection_mode(Gtk::SELECTION_SINGLE);
    m_mlist.set_activate_on_single_click(false); // single click selects; dbl/Enter launches
    m_mlist.get_style_context()->add_class("mlist");
    m_mlist.signal_row_selected().connect(
        sigc::mem_fun(*this, &MainWindow::on_mlist_row_selected));
    m_mlist.signal_row_activated().connect(
        sigc::mem_fun(*this, &MainWindow::on_mlist_row_activated));
    m_scrolled_mlist.add(m_mlist);
    m_scrolled_mlist.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    if (auto adj = m_scrolled_mlist.get_vadjustment()) {
        adj->signal_value_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_mlist));
        adj->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_mlist));
    }

    // Stack children: the hidden TreeView ("table") backs data/selection; the
    // visible views are the modern list and the cover grid.
    m_view_stack.add(m_scrolled_games, "table");
    m_view_stack.add(m_scrolled_mlist, "list");
    m_view_stack.add(m_scrolled_grid, "grid");
    m_view_stack.set_visible_child("list");

    // === Layout: sidebar | (chips, views, detail dock) ===
    m_chips_box.set_margin_start(12);
    m_chips_box.set_margin_end(12);
    m_chips_box.set_margin_top(8);
    m_chips_box.get_style_context()->add_class("chips-bar");
    m_right_box.pack_start(m_chips_box, Gtk::PACK_SHRINK);
    // A resizable split between the game views and the detail dock. The view
    // stack takes the extra space (resize=true); the dock keeps its requested
    // size (resize=false) but can be dragged. set_dock_position() flips the
    // paned orientation to move the dock between bottom and right.
    m_details_scroll.add(m_details_box);
    m_details_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    m_content_paned.pack1(m_view_stack, true, false);
    m_content_paned.pack2(m_details_scroll, false, false);
    m_right_box.pack_start(m_content_paned, Gtk::PACK_EXPAND_WIDGET);

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
    
    // Scan progress widgets : hidden until a scan is running
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
    // actions on the right : matching the design mockup.
    m_headerbar.set_show_close_button(true);

    auto* brand = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    Gtk::Widget* logo = nullptr;
    try {
        auto pix = Gdk::Pixbuf::create_from_file(AppContext::get_asset_path("logo.svg"), 30, 30);
        auto* img = Gtk::make_managed<Gtk::Image>(pix);
        logo = img;
    } catch (...) {
        auto* box = Gtk::make_managed<Gtk::Box>();
        box->get_style_context()->add_class("brand-logo");
        box->set_size_request(30, 30);
        logo = box;
    }
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

    m_btn_favorites.set_label("★");
    m_btn_favorites.set_tooltip_text(_("Show favourites only"));
    m_btn_favorites.get_style_context()->add_class("fav-toggle");
    m_btn_favorites.signal_toggled().connect([this] {
        if (!m_suppress_fav_toggle) set_favorites_only(m_btn_favorites.get_active());
    });

    auto* view_seg = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL);
    view_seg->get_style_context()->add_class("linked");
    view_seg->pack_start(m_btn_view_list);
    view_seg->pack_start(m_btn_view_grid);

    // Cards-per-row segmented control (shown only in grid view).
    m_btn_cols3.set_label("3");
    m_btn_cols4.set_label("4");
    m_btn_cols5.set_label("5");
    m_btn_cols3.set_tooltip_text(_("3 cards per row"));
    m_btn_cols4.set_tooltip_text(_("4 cards per row"));
    m_btn_cols5.set_tooltip_text(_("5 cards per row"));
    m_btn_cols3.signal_toggled().connect([this] {
        if (!m_suppress_cols_toggle && m_btn_cols3.get_active()) set_grid_columns(3);
    });
    m_btn_cols4.signal_toggled().connect([this] {
        if (!m_suppress_cols_toggle && m_btn_cols4.get_active()) set_grid_columns(4);
    });
    m_btn_cols5.signal_toggled().connect([this] {
        if (!m_suppress_cols_toggle && m_btn_cols5.get_active()) set_grid_columns(5);
    });
    m_grid_cols_seg.get_style_context()->add_class("linked");
    m_grid_cols_seg.pack_start(m_btn_cols3);
    m_grid_cols_seg.pack_start(m_btn_cols4);
    m_grid_cols_seg.pack_start(m_btn_cols5);
    // Visibility is driven by the view mode (set_view_mode), applied after the
    // header's show_all so the children are realised before we may hide the box.

    // Detail-dock position toggle: released = bottom, pressed = right.
    m_btn_dock_toggle.set_label("▐");
    m_btn_dock_toggle.set_tooltip_text(_("Dock details to the right"));
    m_btn_dock_toggle.signal_toggled().connect([this] {
        set_dock_position(m_btn_dock_toggle.get_active() ? "right" : "bottom");
    });

    // Packed end -> rightmost first: menu, settings, scan, favourites, view toggle.
    m_headerbar.pack_end(m_menu_button);
    m_headerbar.pack_end(m_btn_settings);
    m_headerbar.pack_end(m_button_scan);
    m_headerbar.pack_end(m_btn_favorites);
    m_headerbar.pack_end(m_btn_dock_toggle);
    m_combo_sort.append("default",  _("By system"));
    m_combo_sort.append("name",     _("Name (A–Z)"));
    m_combo_sort.append("year",     _("Newest first"));
    m_combo_sort.append("yearAsc",  _("Oldest first"));
    m_combo_sort.append("played",   _("Recently played"));
    m_combo_sort.append("hiscore",  _("Highscore first"));
    m_combo_sort.set_active_id("default");
    m_combo_sort.set_tooltip_text(_("Sort"));
    m_combo_sort.signal_changed().connect([this] {
        std::string id = m_combo_sort.get_active_id();
        m_sort_mode = id == "name"    ? SortMode::Name
                    : id == "year"    ? SortMode::Year
                    : id == "yearAsc" ? SortMode::YearAsc
                    : id == "played"  ? SortMode::RecentlyPlayed
                    : id == "hiscore" ? SortMode::Highscore
                                      : SortMode::Default;
        filter_games();
    });
    m_headerbar.pack_end(m_combo_sort);
    m_headerbar.pack_end(m_grid_cols_seg);
    m_headerbar.pack_end(*view_seg);

    set_titlebar(m_headerbar);
    m_headerbar.show_all();

    // === FBNeo update banner (hidden until the startup check finds one) ===
    m_fbneo_update_infobar.set_message_type(Gtk::MESSAGE_INFO);
    m_fbneo_update_infobar.set_show_close_button(true);
    m_fbneo_update_infobar.set_no_show_all(true);
    dynamic_cast<Gtk::Container*>(m_fbneo_update_infobar.get_content_area())->add(m_fbneo_update_label);
    m_fbneo_update_label.show();
    m_fbneo_update_infobar.add_button(_("Download"), Gtk::RESPONSE_OK);
    m_fbneo_update_infobar.signal_response().connect(
        sigc::mem_fun(*this, &MainWindow::on_fbneo_update_infobar_response));
    m_fbneo_update_infobar.hide();

    // === Packing ===
    m_main_box.pack_start(m_fbneo_update_infobar, Gtk::PACK_SHRINK);

    // Result of a score submission. An infobar rather than a dialog: the
    // player has just quit a game and is on their way somewhere else, so the
    // news must be readable without being dismissed first.
    m_hiscore_infobar.set_message_type(Gtk::MESSAGE_INFO);
    m_hiscore_infobar.set_show_close_button(true);
    m_hiscore_infobar.set_no_show_all(true);
    dynamic_cast<Gtk::Container*>(m_hiscore_infobar.get_content_area())
        ->add(m_hiscore_infobar_label);
    m_hiscore_infobar_label.show();
    m_hiscore_infobar.signal_response().connect(
        [this](int) { m_hiscore_infobar.hide(); });
    m_hiscore_infobar.hide();
    m_main_box.pack_start(m_hiscore_infobar, Gtk::PACK_SHRINK);
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
            m_status_label.set_text(_("Database empty - use 'Update DAT' button to load games"));
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
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_rom_manager));
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
        m_status_label.set_text(_("Download completed successfully!"));
        
        // Hide the completion message after 5 seconds
        Glib::signal_timeout().connect([this]() {
            m_status_label.set_text("");
            return false; // Don't repeat the timeout
        }, 5000);
    });
    
    // Connect ROM scan dispatchers
    m_scan_progress_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_scan_progress));
    m_scan_finished_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_scan_finished));

    m_fbneo_update_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_fbneo_update_check_result));
    m_hiscore_supported_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_supported_ready));
    m_hiscore_top_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_top_ready));
    m_hiscore_result_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_result_ready));
    m_hiscore_refresh_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_refresh_done));
    // Rafraîchissement de fond : sans lui, le score d'un autre joueur
    // n'apparaîtrait qu'au prochain démarrage du lanceur.
    Glib::signal_timeout().connect_seconds([this]() {
        refresh_hiscore_data_async(false);
        return true;
    }, 15 * 60);
    refresh_hiscore_data_async(false);
    m_screenshot_found_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_screenshot_batch_found));
    
    // Removed filter population dispatcher

    // === Final setup ===
    if (progress_callback) progress_callback(0.95, "Finalizing interface...");
    show_all_children();

    // Activate the modern list view. This must run AFTER show_all_children():
    // Gtk::Stack::set_visible_child() is ignored while the target child is not yet
    // shown, so an earlier call would leave the hidden TreeView ("table") on screen
    // until the user toggled the view.
    set_view_mode(false);

    // Apply the persisted detail-dock position now that the widget tree exists.
    set_dock_position(m_dock_position);
    // Sync the cards-per-row selector to the persisted value.
    set_grid_columns(m_grid_columns);
    // Start the background cover-art loader (disk I/O + PNG decode off the UI thread).
    start_art_thread();

    // One GitHub API call, off-thread : never blocks startup, and stays silent
    // unless it actually finds something newer than what was downloaded here.
    check_fbneo_update_async();

    if (progress_callback) progress_callback(1.0, "Ready!");
    std::cout << "[DEBUG] MainWindow constructor completed" << std::endl;
}

MainWindow::~MainWindow() {
    // Avant tout démontage. Le verrou garantit qu'aucun ouvrier n'est en
    // train de tester le drapeau puis d'émettre : il attend ici, voit le
    // drapeau tombé, et renonce.
    {
        std::lock_guard<std::mutex> lock(m_alive_token->mutex);
        m_alive_token->alive = false;
    }

    // Clean up scan thread
    if (m_scan_thread.joinable()) {
        m_scan_cancelled = true;
        m_scan_thread.join();
    }
    
    // Disconnect timeout connection
    if (m_search_timeout_connection.connected()) {
        m_search_timeout_connection.disconnect();
    }

    // Stop the background art loader and wait for it to finish.
    m_art_thread_stop.store(true);
    m_art_req_cv.notify_all();
    if (m_art_thread.joinable()) {
        m_art_thread.join();
    }
}

void MainWindow::on_game_selected() {
    auto iter = m_treeview_games.get_selection()->get_selected();
    if (!iter) return;
    show_game_details(*iter);
}

void MainWindow::show_game_details(const Gtk::TreeModel::Row& row) {
    std::string name = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    
    // Get system prefix for file lookup
    std::string system_prefix = get_fbneo_system_prefix(system);
    
    // Load both artworks (Title screen and in-game Preview) at a legible size,
    // preserving aspect ratio inside a bounding box.
    std::string filename_with_prefix = system_prefix + name;
    auto load_art = [&](Gtk::Image& img, const std::string& dir, int max_w, int max_h) {
        if (dir.empty()) { img.hide(); return; }
        std::string path = dir + "/" + filename_with_prefix + ".png";
        try {
            if (std::filesystem::exists(path)) {
                auto pix = Gdk::Pixbuf::create_from_file(path, max_w, max_h, true);
                if (pix) { img.set(pix); img.show(); return; }
            }
        } catch (...) {}
        img.hide();
    };
    load_art(m_title_image,   m_settings_panel.get_titles_path(),   320, 150);
    load_art(m_preview_image, m_settings_panel.get_previews_path(), 320, 240);

    std::string manufacturer = Glib::ustring(row[m_columns.m_col_manufacturer]).raw();
    std::string year         = Glib::ustring(row[m_columns.m_col_year]).raw();
    std::string status       = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool        fav          = row[m_columns.m_col_favorite];

    // Extra metadata straight from the DAT (already in the model).
    std::string video_type    = Glib::ustring(row[m_columns.m_col_video_type]).raw();
    std::string orientation   = Glib::ustring(row[m_columns.m_col_orientation]).raw();
    std::string width         = Glib::ustring(row[m_columns.m_col_width]).raw();
    std::string height        = Glib::ustring(row[m_columns.m_col_height]).raw();
    std::string aspect        = Glib::ustring(row[m_columns.m_col_aspect]).raw();
    std::string driver_status = Glib::ustring(row[m_columns.m_col_driver_status]).raw();
    std::string cloneof       = Glib::ustring(row[m_columns.m_col_cloneof]).raw();
    std::string comment       = Glib::ustring(row[m_columns.m_col_comment]).raw();

    m_label_title.set_text(title.empty() ? name : title);
    {
        std::vector<std::string> bits;
        if (!manufacturer.empty()) bits.push_back(manufacturer);
        if (!year.empty())         bits.push_back(year);
        if (!system.empty())       bits.push_back(system);
        std::string line;
        for (size_t i = 0; i < bits.size(); ++i)
            line += (i ? "  \u00b7  " : "") + bits[i];
        m_label_meta.set_text(line);
    }

    // Full metadata block: one "Label: value" line per known field. Empty fields
    // are skipped so the block stays tight. Genre / players / synopsis will slot
    // in here once the offline metadata import (history.dat, catver.ini…) lands.
    // Les caracteristiques vont dans la grille ; l'etiquette ne garde que le
    // commentaire du DAT, qui est une phrase et non un couple intitule-valeur.
    for (auto* c : m_specs_grid.get_children()) m_specs_grid.remove(*c);
    int spec_row = 0;
    auto add_spec = [&](const std::string& label, const std::string& value) {
        if (value.empty()) return;
        auto* k = Gtk::make_managed<Gtk::Label>(label);
        k->set_xalign(0.0f);
        k->get_style_context()->add_class("spec-key");
        auto* v = Gtk::make_managed<Gtk::Label>(value);
        v->set_xalign(0.0f);
        v->set_ellipsize(Pango::ELLIPSIZE_END);
        v->get_style_context()->add_class("spec-val");
        m_specs_grid.attach(*k, 0, spec_row, 1, 1);
        m_specs_grid.attach(*v, 1, spec_row, 1, 1);
        spec_row++;
    };
    add_spec(_("System"),       system);
    add_spec(_("Manufacturer"), manufacturer);
    add_spec(_("Year"),         year);
    add_spec(_("ROM name"),     name);
    if (!cloneof.empty()) add_spec(_("Clone of"), cloneof);
    if (!width.empty() && !height.empty())
        add_spec(_("Resolution"), width + " x " + height);
    add_spec(_("Orientation"),  orientation);
    add_spec(_("Video"),        video_type);
    add_spec(_("Aspect"),       aspect);
    add_spec(_("Driver"),       driver_status);

    Game stats = m_database->getGame(name, system);
    if (stats.play_time_secs > 0 || stats.play_count > 0) {
        add_spec(_("Last session"),    format_duration(stats.last_session_secs));
        add_spec(_("Longest session"), format_duration(stats.longest_session_secs));
        add_spec(_("Total played"),    format_duration(stats.play_time_secs));
        if (stats.play_count > 0)
            add_spec(_("Times played"), std::to_string(stats.play_count));
    }
    m_specs_grid.show_all();

    std::string info;
    if (!comment.empty()) info = escape_markup(comment);
    m_label_info.set_markup(info);

    // Leaderboard: painted empty now, filled in when the network answers.
    if (game_ranks_online(system, name)) {
        // Lecture du cache, sans requête. Le lot complet est chargé au
        // démarrage : une requête par jeu cliqué rendait la navigation
        // poussive, chacune pouvant caler le temps du délai de connexion.
        m_hiscore_box.show();
        m_hiscore_head.show_all();
        m_hiscore_grid.show_all();
        show_cached_board(system, name);
    } else {
        m_hiscore_box.hide();
    }

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
    if (game_ranks_online(system, name))
        add_pill("◆ " + _("Highscore"), "pill-hiscore");
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

void MainWindow::set_dock_position(const std::string& pos) {
    const bool right = (pos == "right");
    m_dock_position = right ? "right" : "bottom";

    // The outer split runs horizontally when the dock is on the right, vertically
    // when it is at the bottom. The dock's own A|B|C layout is the opposite so it
    // fills the available axis: stacked (fill height) on the right, in a row (fill
    // width) at the bottom. The two artworks stack on the right, sit side-by-side
    // at the bottom.
    m_content_paned.set_orientation(right ? Gtk::ORIENTATION_HORIZONTAL
                                          : Gtk::ORIENTATION_VERTICAL);
    m_details_box.set_orientation(right ? Gtk::ORIENTATION_VERTICAL
                                        : Gtk::ORIENTATION_HORIZONTAL);
    m_detail_image_wrap.set_orientation(right ? Gtk::ORIENTATION_VERTICAL
                                              : Gtk::ORIENTATION_HORIZONTAL);
    m_details_box.set_spacing(right ? 16 : 24);

    // TOUS les alignements du volet sont decides ici, et nulle part ailleurs.
    // Ils etaient auparavant poses a la construction puis reecrits par cette
    // fonction, si bien qu'une retouche visible restait sans effet : l'artwork
    // repassait a gauche pendant que le reste se centrait.
    //
    // Dock lateral : tout est empile, donc tout partage une colonne de 360 px
    // centree. Sans cette borne, le volet s'etire a la largeur de la fenetre
    // et le contenu se disperse.
    // Dock bas : les trois groupes sont cote a cote, chacun garde sa largeur
    // naturelle et s'aligne a gauche.
    const auto lead = right ? Gtk::ALIGN_CENTER : Gtk::ALIGN_START;
    m_detail_image_wrap.set_halign(lead);
    m_detail_text_col.set_halign(lead);
    m_detail_text_col.set_hexpand(false);
    m_detail_text_col.set_size_request(right ? 360 : -1, -1);
    m_dock_pills.set_halign(lead);
    m_specs_grid.set_halign(Gtk::ALIGN_FILL);
    m_detail_actions.set_halign(lead);
    m_detail_actions_col.set_halign(lead);
    m_label_title.set_xalign(right ? 0.5f : 0.0f);
    m_label_meta.set_xalign(right ? 0.5f : 0.0f);
    m_label_title.set_justify(right ? Gtk::JUSTIFY_CENTER : Gtk::JUSTIFY_LEFT);
    m_label_meta.set_justify(right ? Gtk::JUSTIFY_CENTER : Gtk::JUSTIFY_LEFT);

    // Give the dock a sensible floor so the paned doesn't collapse it: a column
    // on the right, a short band at the bottom.
    if (right) {
        m_details_scroll.set_min_content_width(340);
        m_details_scroll.set_min_content_height(-1);
    } else {
        m_details_scroll.set_min_content_width(-1);
        m_details_scroll.set_min_content_height(270); // fits the side-by-side artworks
    }

    m_btn_dock_toggle.set_tooltip_text(right ? _("Dock details at the bottom")
                                             : _("Dock details to the right"));
    if (m_btn_dock_toggle.get_active() != right) {
        m_btn_dock_toggle.set_active(right); // reflect programmatic changes (e.g. load)
    }

    save_launch_prefs();
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
    std::string fbneo_rom_name = get_fbneo_system_prefix(game_system) + rom_name;
    
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
    // No "-joy": that flag makes FBNeo map the pad onto EVERY player, so a
    // single controller ends up driving both P1 and P2 coin/start : one press
    // inserts two credits and starts a two-player game. Verified by running
    // the emulator both ways: without it, player 1 gets the pad in full
    // (D-pad, buttons, coin, start) and player 2 stays on the keyboard, which
    // is what a one-pad setup should do. The pad works fine without the flag.
    if (m_launch_fullscreen)   launch_args.push_back("-fullscreen");
    if (m_launch_integerscale) launch_args.push_back("-integerscale");
    launch_args.push_back(fbneo_rom_name);

    std::cout << "Launching " << game_system << " game:";
    for (const auto& a : launch_args) std::cout << " " << a;
    std::cout << std::endl;

    // Record launch (last_played + play_count)
    m_database->recordLaunch(rom_name, game_system);

    std::time_t launch_time = std::time(nullptr);
    std::string previews_dir = m_settings_panel.get_previews_path();
    std::string titles_dir = m_settings_panel.get_titles_path();

    // Repair a config left conflicting by a previous session before the game
    // starts, so the fix takes effect from this launch rather than the next.
    ControllerManager::fix_player2_input_conflicts(fbneo_rom_name);
    // Wheels, paddles, dials and pointers: FBNeo leaves them on the keyboard,
    // and no per-player default can reach them (see apply_analog_bindings).
    if (m_controller_profiles.count(m_active_controller_profile))
        ControllerManager::apply_analog_bindings(
            fbneo_rom_name, m_controller_profiles.at(m_active_controller_profile));

    // Snapshot of the score table BEFORE play. Without it the server cannot
    // tell what this session achieved from what the table already held : a
    // fresh table ships with factory scores that belong to nobody.
    std::string hi_before = read_file_bytes(fbneo_hiscore_path(fbneo_rom_name));
    // Read here, on the GTK thread, and carried into the watcher: the panel
    // must not be touched from there. Empty means "do not send".
    std::string hiscore_player = m_settings_panel.is_hiscore_enabled()
                               ? m_settings_panel.get_hiscore_player() : std::string();
    std::string hiscore_country = m_settings_panel.get_hiscore_country();

    pid_t pid = spawn_process(launch_args);
    if (pid > 0) {
        // Detached watcher thread: waits for process exit, records playtime,
        // then checks whether FBNeo's own F6 screenshot hotkey was used during
        // the session : if so, offer to use the capture(s) as artwork.
        std::thread([this, pid, rom_name, game_system, fbneo_rom_name, previews_dir, titles_dir, launch_time, hi_before, hiscore_player, hiscore_country,
                     alive = m_alive_token]() {
            watch_playtime(pid, m_database, rom_name, game_system);
            // La fenêtre a pu être fermée pendant la partie.
            {
                std::lock_guard<std::mutex> live(alive->mutex);
                if (!alive->alive) return;
            }
            // FBNeo writes the .hi on exit, so this must come after the wait.
            submit_session_score(game_system, rom_name, fbneo_rom_name, hi_before, hiscore_player, hiscore_country);
            // FBNeo has just written config/games/<rom>.ini on exit : this is
            // the only moment a complete file exists to repair.
            ControllerManager::fix_player2_input_conflicts(fbneo_rom_name);
            // Same reason for the analog inputs. On a game's very first run the
            // file did not exist before launch, so there was nothing to bind
            // and the wheel stayed on the keyboard; repairing it here means it
            // is right from the second run on, without waiting for the next
            // launch to notice. The first run of a new analog game is
            // unavoidably on the keyboard: FBNeo only reveals a game's input
            // list by writing this file, and it does that on exit.
            if (m_controller_profiles.count(m_active_controller_profile))
                ControllerManager::apply_analog_bindings(
                    fbneo_rom_name,
                    m_controller_profiles.at(m_active_controller_profile));
            std::cout << "[SCREENSHOT] session ended for " << fbneo_rom_name
                      << " previews_dir=" << previews_dir << " titles_dir=" << titles_dir
                      << " launch_time=" << launch_time << std::endl;
            if (previews_dir.empty() && titles_dir.empty()) {
                std::cout << "[SCREENSHOT] both dirs empty, skipping scan" << std::endl;
                return;
            }
            auto shots = find_session_screenshots(fbneo_rom_name, launch_time);
            std::cout << "[SCREENSHOT] scan dir=" << get_fbneo_screenshots_dir()
                      << " found " << shots.size() << " fresh capture(s)" << std::endl;
            if (shots.empty()) return;
            std::lock_guard<std::mutex> lock(m_screenshot_queue_mutex);
            m_screenshot_queue.push_back({fbneo_rom_name, shots, previews_dir, titles_dir});
            std::cout << "[SCREENSHOT] emitting dispatcher, queue size now "
                      << m_screenshot_queue.size() << std::endl;
            m_screenshot_found_dispatcher.emit();
        }).detach();
    }
}

bool MainWindow::normalize_artwork_file(const std::string& path, int target_w, int target_h) {
    try {
        auto src = Gdk::Pixbuf::create_from_file(path);
        int w = src->get_width();
        int h = src->get_height();
        if (w <= 0 || h <= 0) return false;

        // Fit the whole capture inside the target box : never crop it. Most
        // systems' screens are landscape (~4:3) while the title slot is a
        // portrait 384x512 frame: cropping to fill would cut off a large
        // chunk of the actual title screen instead of just trimming margins.
        // Pad with black bars to reach the exact target size instead.
        double scale = std::min(target_w / (double)w, target_h / (double)h);
        int scaled_w = std::max(1, (int)std::lround(w * scale));
        int scaled_h = std::max(1, (int)std::lround(h * scale));
        auto scaled = src->scale_simple(scaled_w, scaled_h, Gdk::INTERP_BILINEAR);

        auto canvas = Gdk::Pixbuf::create(scaled->get_colorspace(), scaled->get_has_alpha(),
                                           scaled->get_bits_per_sample(), target_w, target_h);
        canvas->fill(0x000000ff);
        int x = (target_w - scaled_w) / 2;
        int y = (target_h - scaled_h) / 2;
        scaled->copy_area(0, 0, scaled_w, scaled_h, canvas, x, y);
        canvas->save(path, "png");
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] normalize_artwork_file failed for " << path << ": " << e.what() << std::endl;
        return false;
    }
}

std::string MainWindow::get_fbneo_screenshots_dir() {
    // Matches FBNeo's own SDL_GetPrefPath("fbneo", "screenshots") on Linux // untouched, upstream behavior behind the existing F6 hotkey.
    const char* xdg = getenv("XDG_DATA_HOME");
    std::string base = (xdg && *xdg) ? xdg : (std::string(getenv("HOME")) + "/.local/share");
    return base + "/fbneo/screenshots";
}

std::vector<std::string> MainWindow::find_session_screenshots(const std::string& fbneo_rom_name, std::time_t launch_time) {
    std::vector<std::string> result;
    std::string dir = get_fbneo_screenshots_dir();
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) return result;

    std::string prefix = fbneo_rom_name + "-";
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        if (entry.path().extension() != ".png") continue;
        std::time_t mtime = get_file_mtime(entry.path().string());
        if (mtime < 0 || mtime < launch_time) continue; // pre-existing capture from an earlier session
        result.push_back(entry.path().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

void MainWindow::on_screenshot_batch_found() {
    std::cout << "[SCREENSHOT] on_screenshot_batch_found() dispatched on UI thread" << std::endl;
    for (;;) {
        PendingScreenshotBatch batch;
        {
            std::lock_guard<std::mutex> lock(m_screenshot_queue_mutex);
            if (m_screenshot_queue.empty()) {
                std::cout << "[SCREENSHOT] queue empty, done" << std::endl;
                return;
            }
            batch = m_screenshot_queue.front();
            m_screenshot_queue.pop_front();
        }
        std::cout << "[SCREENSHOT] showing dialog for " << batch.fbneo_rom_name
                  << " with " << batch.screenshot_paths.size() << " capture(s)" << std::endl;

        int result;
        try {
            ScreenshotAssignDialog dialog(*this, batch.screenshot_paths);
            result = dialog.run();
            std::cout << "[SCREENSHOT] dialog closed, response=" << result << std::endl;
            if (result != Gtk::RESPONSE_OK) continue;

            std::error_code ec;
            std::string title_choice = dialog.get_title_choice();
            if (!title_choice.empty() && !batch.titles_dir.empty()) {
                std::filesystem::create_directories(batch.titles_dir, ec);
                std::string dest = batch.titles_dir + "/" + batch.fbneo_rom_name + ".png";
                std::filesystem::copy_file(title_choice, dest, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) normalize_artwork_file(dest, 384, 512);
                else std::cerr << "[SCREENSHOT] copy_file(title) failed: " << ec.message() << std::endl;
            }
            std::string preview_choice = dialog.get_preview_choice();
            if (!preview_choice.empty() && !batch.previews_dir.empty()) {
                std::filesystem::create_directories(batch.previews_dir, ec);
                std::string dest = batch.previews_dir + "/" + batch.fbneo_rom_name + ".png";
                std::filesystem::copy_file(preview_choice, dest, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) normalize_artwork_file(dest, 512, 384);
                else std::cerr << "[SCREENSHOT] copy_file(preview) failed: " << ec.message() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[SCREENSHOT] EXCEPTION in on_screenshot_batch_found: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[SCREENSHOT] UNKNOWN EXCEPTION in on_screenshot_batch_found" << std::endl;
        }
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
        m_status_label.set_text(_("Downloading preview for ") + game_title + "...");
        m_thumbnail_downloader.download_single_artwork(game_name, game_system, previews_dir, ThumbnailDownloader::ArtworkType::Previews, single_download_callback);
        
        // Wait a moment before downloading title
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // Download title if directory is configured - use ROM name and system
    if (!titles_dir.empty() && !m_thumbnail_downloader.is_downloading()) {
        std::cout << "[INFO] Downloading title for: " << game_title << " (ROM: " << game_name << ", System: " << game_system << ")" << std::endl;
        m_status_label.set_text(_("Downloading title for ") + game_title + "...");
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
        m_status_label.set_text(_("Error: No ROM directories defined"));
        m_status_label.show();
        return;
    }
    
    // Ensure database is loaded with DAT data (cheap count query, no full load)
    if (m_database->getGameCount() == 0) {
        std::string dat_path = m_settings_panel.get_dat_path();
        if (dat_path.empty()) {
            m_status_label.set_text(_("Error: No DAT path defined"));
            m_status_label.show();
            return;
        }
        
        std::cout << "[INFO] Reloading DAT files to database..." << std::endl;
        if (!DatParser::parseAllDatsToDatabase(dat_path, m_database)) {
            m_status_label.set_text(_("Error: Failed to load DAT files"));
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
        "The game database will be reloaded from the DAT files.\nGames whose ROM definition is unchanged keep their status \nonly new or changed games are re-checked on the next scan.\n\nDo you want to continue?",
        "🔄");

    if (!confirm_dialog.show_and_confirm()) {
        std::cout << "[INFO] Update DAT cancelled by user" << std::endl;
        return;
    }

    do_update_dat();
}

void MainWindow::do_update_dat() {
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
    auto dialog = Gtk::Dialog(_("Settings"), *this, Gtk::DIALOG_MODAL);
    dialog.set_default_size(800, 500);
    dialog.get_content_area()->pack_start(m_settings_panel);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("OK"), Gtk::RESPONSE_OK);

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
        row[m_columns.m_col_hiscore] = game_ranks_online(game.system, game.name)
                                     ? Glib::ustring("\u25cf") : Glib::ustring();
        row[m_columns.m_col_last_played] = game.last_played;
    }

    if (had_sort &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID) {
        m_model_games->set_sort_column(prev_sort_col, prev_sort_order);
    }
    m_treeview_games.set_model(m_model_games);

    // Keep the active custom view (list/grid) in sync with the model.
    refresh_active_view();

    update_status_bar_stats();
}

void MainWindow::set_view_mode(bool grid) {
    m_suppress_view_toggle = true;
    m_btn_view_grid.set_active(grid);
    m_btn_view_list.set_active(!grid);
    m_suppress_view_toggle = false;
    // The cards-per-row selector only makes sense over the grid.
    m_grid_cols_seg.set_visible(grid);
    if (grid) rebuild_grid(); else rebuild_mlist(); // build lazily, only when shown
    m_view_stack.set_visible_child(grid ? "grid" : "list");
}

void MainWindow::set_grid_columns(int n) {
    if (n < 3) n = 3; else if (n > 5) n = 5;
    m_grid_columns = n;

    // Reflect the choice in the segmented control without re-triggering it.
    m_suppress_cols_toggle = true;
    m_btn_cols3.set_active(n == 3);
    m_btn_cols4.set_active(n == 4);
    m_btn_cols5.set_active(n == 5);
    m_suppress_cols_toggle = false;

    // Exact column count: the flowbox lays out precisely n cards per line, so a
    // long title wraps inside its (fixed-share) card instead of stretching it.
    m_flowbox.set_min_children_per_line(n);
    m_flowbox.set_max_children_per_line(n);

    save_launch_prefs();
}

void MainWindow::refresh_active_view() {
    const auto v = m_view_stack.get_visible_child_name();
    if (v == "grid")      rebuild_grid();
    else if (v == "list") rebuild_mlist();
}

// Pure, thread-safe: no GTK, no settings access : safe to call from the art
// worker thread. Takes the artwork directories explicitly (captured on the main
// thread when the request is queued).
static std::string resolve_art_path(const std::string& name, const std::string& system,
                                    const std::string& previews_dir,
                                    const std::string& titles_dir) {
    std::string pfx = get_fbneo_system_prefix(system);

    for (const std::string& dir : {previews_dir, titles_dir}) {
        if (dir.empty()) continue;
        std::string path = dir + "/" + pfx + name + ".png";
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) return path;
    }
    return "";
}

// Drop parenthesised qualifiers such as "(World 910522)" or "(set 1)" so cards
// stay compact; the untouched title is still shown in the detail dock.
static std::string strip_parentheticals(const std::string& s) {
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(') { ++depth; continue; }
        if (c == ')') { if (depth > 0) --depth; continue; }
        if (depth == 0) out += c;
    }
    // Collapse the whitespace the removed groups left behind.
    std::string clean;
    bool prev_space = false;
    for (char c : out) {
        bool is_space = (c == ' ' || c == '\t');
        if (is_space && (prev_space || clean.empty())) continue;
        clean += c;
        prev_space = is_space;
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t' || clean.back() == '-'))
        clean.pop_back();
    return clean.empty() ? s : clean;
}

Gtk::Widget* MainWindow::make_game_card(const Gtk::TreeModel::Row& row) {
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title  = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string status = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool fav = row[m_columns.m_col_favorite];
    // Show the human title (e.g. "Metal Slug"), not the ROM name ("mslug"),
    // shortened to its base form to keep the card narrow.
    std::string display = strip_parentheticals(title.empty() ? name : title);

    const char* dot = status == "available" ? "#41d08a"
                    : status == "incorrect" ? "#f0b54a"
                    : status == "missing"   ? "#5a6272" : "#939aab";

    auto* card = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    card->get_style_context()->add_class("game-card");

    // Art holder: shows a titled placeholder until the background worker resolves
    // and decodes the PNG and swaps it in (see queue_art / on_art_ready). No disk
    // access happens here on the UI thread.
    auto* art_holder = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    art_holder->get_style_context()->add_class("card-art");
    art_holder->get_style_context()->add_class("card-art-empty");
    art_holder->set_size_request(176, 132);
    art_holder->set_halign(Gtk::ALIGN_CENTER);
    auto* ph_lbl = Gtk::make_managed<Gtk::Label>(display);
    ph_lbl->set_line_wrap(true);
    ph_lbl->set_justify(Gtk::JUSTIFY_CENTER);
    ph_lbl->set_max_width_chars(16);
    ph_lbl->set_lines(3);
    ph_lbl->set_ellipsize(Pango::ELLIPSIZE_END);
    ph_lbl->set_valign(Gtk::ALIGN_CENTER);
    ph_lbl->set_vexpand(true);
    ph_lbl->get_style_context()->add_class("card-art-title");
    art_holder->pack_start(*ph_lbl, true, true);
    card->pack_start(*art_holder, Gtk::PACK_SHRINK);
    queue_art(art_holder, name, system, 176, 132);

    auto* meta = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
    meta->get_style_context()->add_class("card-meta");
    auto* nlbl = Gtk::make_managed<Gtk::Label>();
    nlbl->set_text((fav ? "★ " : "") + display);
    // Wrap long titles onto a second line (then ellipsize) instead of letting the
    // label's natural width stretch the card and collapse the row to two columns.
    nlbl->set_line_wrap(true);
    nlbl->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    nlbl->set_lines(2);
    nlbl->set_ellipsize(Pango::ELLIPSIZE_END);
    nlbl->set_max_width_chars(1); // don't request width from text; the cell governs it
    nlbl->set_xalign(0.0f);
    nlbl->get_style_context()->add_class("card-name");
    meta->pack_start(*nlbl, Gtk::PACK_SHRINK);
    auto* slbl = Gtk::make_managed<Gtk::Label>();
    // The ◆ rides on the system line rather than getting a row of its own: a
    // card is 176 px wide, and a second line would push the title out.
    slbl->set_markup("<span foreground=\"" + std::string(dot) + "\">●</span> " +
                     Glib::Markup::escape_text(system) +
                     (game_ranks_online(system, name)
                        ? std::string("  <span foreground=\"#7aa2ff\">◆</span>") : ""));
    slbl->set_ellipsize(Pango::ELLIPSIZE_END);
    slbl->set_max_width_chars(1); // let the cell govern width, not the text
    slbl->set_xalign(0.0f);
    slbl->get_style_context()->add_class("card-sys");
    meta->pack_start(*slbl, Gtk::PACK_SHRINK);
    card->pack_start(*meta, Gtk::PACK_SHRINK);

    return card;
}

// ---- Background cover-art loading (disk I/O + decode never on the UI thread) ----

void MainWindow::start_art_thread() {
    // The dispatcher is created/connected on the main thread; the worker only
    // emit()s it. Start the single worker that services the request queue.
    m_art_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_art_ready));
    m_art_thread = std::thread(&MainWindow::art_worker, this);
}

void MainWindow::queue_art(Gtk::Box* holder, const std::string& name,
                           const std::string& system, int w, int h) {
    // Capture the artwork directories here, on the main thread, so the worker
    // never touches the settings panel.
    std::string pdir = m_settings_panel.get_previews_path();
    std::string tdir = m_settings_panel.get_titles_path();
    if (pdir.empty() && tdir.empty()) return; // nowhere to look -> keep placeholder

    {
        std::lock_guard<std::mutex> lk(m_art_req_mutex);
        m_art_requests.push_back({m_art_generation.load(), holder, name, system,
                                  std::move(pdir), std::move(tdir), w, h});
    }
    m_art_req_cv.notify_one();
}

void MainWindow::art_worker() {
    for (;;) {
        ArtRequest req;
        {
            std::unique_lock<std::mutex> lk(m_art_req_mutex);
            m_art_req_cv.wait(lk, [this] {
                return m_art_thread_stop.load() || !m_art_requests.empty();
            });
            if (m_art_thread_stop.load()) return;
            req = std::move(m_art_requests.back()); // newest first: what's on screen
            m_art_requests.pop_back();
        }
        // Skip work for requests whose view has already been rebuilt.
        if (req.gen != m_art_generation.load()) continue;

        std::string path = resolve_art_path(req.name, req.system,
                                            req.previews_dir, req.titles_dir);
        if (path.empty()) continue; // no art -> the placeholder stays

        Glib::RefPtr<Gdk::Pixbuf> pix;
        try {
            pix = Gdk::Pixbuf::create_from_file(path, req.w, req.h, true);
        } catch (...) { pix.reset(); }
        if (!pix) continue;

        {
            std::lock_guard<std::mutex> lk(m_art_res_mutex);
            m_art_results.push_back({req.gen, req.holder, std::move(pix)});
        }
        m_art_dispatcher.emit(); // wake the main thread to install the pixbuf
    }
}

void MainWindow::on_art_ready() {
    // Runs on the main thread (serialised with rebuilds), so a result whose gen
    // still matches is guaranteed to reference a live holder widget.
    std::deque<ArtResult> batch;
    {
        std::lock_guard<std::mutex> lk(m_art_res_mutex);
        batch.swap(m_art_results);
    }
    const std::uint64_t gen = m_art_generation.load();
    for (auto& r : batch) {
        if (r.gen != gen || !r.holder) continue; // stale: holder already destroyed
        for (auto* c : r.holder->get_children()) r.holder->remove(*c);
        auto* img = Gtk::make_managed<Gtk::Image>(r.pix);
        img->set_halign(Gtk::ALIGN_CENTER);
        img->set_valign(Gtk::ALIGN_CENTER);
        r.holder->get_style_context()->remove_class("card-art-empty");
        r.holder->add(*img);
        r.holder->show_all();
    }
}

void MainWindow::clear_art_queue() {
    // A rebuild is about to destroy the holder widgets: bump the generation so any
    // in-flight request/result is ignored, and drop everything already queued.
    m_art_generation.fetch_add(1);
    { std::lock_guard<std::mutex> lk(m_art_req_mutex); m_art_requests.clear(); }
    { std::lock_guard<std::mutex> lk(m_art_res_mutex); m_art_results.clear(); }
}

void MainWindow::rebuild_grid() {
    clear_art_queue();
    for (auto* c : m_flowbox.get_children()) m_flowbox.remove(*c);
    m_grid_refs.clear();
    m_grid_built = 0;
    append_grid_batch();
}

void MainWindow::append_grid_batch() {
    if (m_batch_lock) return; // adding widgets re-emits the adjustment signals
    auto children = m_model_games->children();
    const int total = static_cast<int>(children.size());
    if (m_grid_built >= total) return;

    m_batch_lock = true;
    const int end = std::min(m_grid_built + kViewBatch, total);
    auto it = children.begin();
    std::advance(it, m_grid_built);
    for (int i = m_grid_built; i < end; ++i, ++it) {
        m_grid_refs.push_back(Gtk::TreeRowReference(m_model_games, m_model_games->get_path(it)));
        m_flowbox.add(*make_game_card(*it));
    }
    m_grid_built = end;
    m_flowbox.show_all_children();
    m_batch_lock = false;
}

// Build the next batch once the viewport is within ~1.5 pages of the bottom.
// The condition also holds when the content is shorter than the viewport, so a
// short first batch keeps filling until the window is full.
void MainWindow::maybe_extend_grid() {
    auto adj = m_scrolled_grid.get_vadjustment();
    if (!adj) return;
    if (adj->get_value() < adj->get_upper() - adj->get_page_size() * 1.5) return;
    append_grid_batch();
}

void MainWindow::on_grid_selection_changed() {
    auto sel = m_flowbox.get_selected_children();
    if (sel.empty()) return;
    int idx = sel[0]->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_grid_refs.size())) return;
    auto& ref = m_grid_refs[idx];
    if (!ref.is_valid()) return;
    auto iter = m_model_games->get_iter(ref.get_path());
    if (!iter) return;
    // Sync the (hidden) treeview selection for Play, and populate the dock directly
    // so it never depends on the treeview's selection-changed signal firing.
    m_treeview_games.get_selection()->select(iter);
    show_game_details(*iter);
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

// ---- Modern list view (styled ListBox rows) ----
Gtk::Widget* MainWindow::make_list_row(const Gtk::TreeModel::Row& row) {
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title  = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string year   = Glib::ustring(row[m_columns.m_col_year]).raw();
    std::string status = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool fav = row[m_columns.m_col_favorite];

    const char* dot = status == "available" ? "#41d08a"
                    : status == "incorrect" ? "#f0b54a"
                    : status == "missing"   ? "#5a6272" : "#939aab";
    const char* pill_cls = status == "available" ? "pill-ok"
                         : status == "incorrect" ? "pill-warn" : "pill-muted";
    std::string status_txt = status == "available" ? _("Available")
                           : status == "incorrect" ? _("Incorrect")
                           : status == "missing"   ? _("Missing") : status;

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    box->get_style_context()->add_class("mlist-row");

    // Thumbnail holder: tinted placeholder until the worker swaps in the PNG.
    auto* thumb = Gtk::make_managed<Gtk::Box>();
    thumb->get_style_context()->add_class("mlist-thumb");
    thumb->get_style_context()->add_class("card-art-empty");
    thumb->set_size_request(52, 39);
    thumb->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*thumb, Gtk::PACK_SHRINK);
    queue_art(thumb, name, system, 52, 39);

    // Name + subtitle.
    auto* nb = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    nb->set_valign(Gtk::ALIGN_CENTER);
    std::string display = title.empty() ? name : title; // human title, not ROM name
    auto* nl = Gtk::make_managed<Gtk::Label>();
    nl->set_markup(std::string(fav ? "★ " : "") + "<b>" + escape_markup(display) + "</b>");
    nl->set_xalign(0.0f);
    nl->set_ellipsize(Pango::ELLIPSIZE_END);
    auto* sl = Gtk::make_managed<Gtk::Label>(name); // ROM name as the discreet subtitle
    sl->set_xalign(0.0f);
    sl->set_ellipsize(Pango::ELLIPSIZE_END);
    sl->get_style_context()->add_class("mlist-sub");
    nb->pack_start(*nl, Gtk::PACK_SHRINK);
    nb->pack_start(*sl, Gtk::PACK_SHRINK);
    box->pack_start(*nb, Gtk::PACK_EXPAND_WIDGET);

    // System.
    auto* syl = Gtk::make_managed<Gtk::Label>(system);
    syl->set_xalign(0.0f);
    syl->set_size_request(130, -1);
    syl->set_ellipsize(Pango::ELLIPSIZE_END);
    syl->get_style_context()->add_class("mlist-sys");
    syl->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*syl, Gtk::PACK_SHRINK);

    // Year.
    auto* yl = Gtk::make_managed<Gtk::Label>(year);
    yl->set_xalign(0.0f);
    yl->set_size_request(48, -1);
    yl->get_style_context()->add_class("mlist-year");
    yl->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*yl, Gtk::PACK_SHRINK);

    // Status pill.
    auto* pill = Gtk::make_managed<Gtk::Label>();
    pill->set_markup("<span foreground=\"" + std::string(dot) + "\">●</span> " +
                     Glib::Markup::escape_text(status_txt));
    pill->get_style_context()->add_class("pill");
    pill->get_style_context()->add_class(pill_cls);
    pill->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*pill, Gtk::PACK_SHRINK);

    // Highscore pill : only on games the service can actually rank. Placed
    // last so its presence or absence never shifts the columns above it.
    if (game_ranks_online(system, name)) {
        auto* hi = Gtk::make_managed<Gtk::Label>();
        hi->set_markup("<span foreground=\"#7aa2ff\">\u25c6</span> " +
                       Glib::Markup::escape_text(_("Highscore")));
        hi->get_style_context()->add_class("pill");
        hi->get_style_context()->add_class("pill-hiscore");
        hi->set_valign(Gtk::ALIGN_CENTER);
        hi->set_tooltip_text(_("This game's scores can be ranked online."));
        box->pack_start(*hi, Gtk::PACK_SHRINK);
    }

    return box;
}

void MainWindow::rebuild_mlist() {
    clear_art_queue();
    for (auto* c : m_mlist.get_children()) m_mlist.remove(*c);
    m_mlist_refs.clear();
    m_mlist_built = 0;
    append_mlist_batch();
}

void MainWindow::append_mlist_batch() {
    if (m_batch_lock) return; // adding widgets re-emits the adjustment signals
    auto children = m_model_games->children();
    const int total = static_cast<int>(children.size());
    if (m_mlist_built >= total) return;

    m_batch_lock = true;
    const int end = std::min(m_mlist_built + kViewBatch, total);
    auto it = children.begin();
    std::advance(it, m_mlist_built);
    for (int i = m_mlist_built; i < end; ++i, ++it) {
        m_mlist.add(*make_list_row(*it));
        m_mlist_refs.push_back(Gtk::TreeRowReference(m_model_games, m_model_games->get_path(it)));
    }
    m_mlist_built = end;
    m_mlist.show_all_children();
    m_batch_lock = false;
}

void MainWindow::maybe_extend_mlist() {
    auto adj = m_scrolled_mlist.get_vadjustment();
    if (!adj) return;
    if (adj->get_value() < adj->get_upper() - adj->get_page_size() * 1.5) return;
    append_mlist_batch();
}

void MainWindow::on_mlist_row_selected(Gtk::ListBoxRow* row) {
    if (!row) return;
    int idx = row->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_mlist_refs.size())) return;
    auto& ref = m_mlist_refs[idx];
    if (!ref.is_valid()) return;
    auto iter = m_model_games->get_iter(ref.get_path());
    if (!iter) return;
    m_treeview_games.get_selection()->select(iter);
    show_game_details(*iter); // populate the dock directly (deterministic)
}

void MainWindow::on_mlist_row_activated(Gtk::ListBoxRow* row) {
    if (!row) return;
    int idx = row->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_mlist_refs.size())) return;
    auto& ref = m_mlist_refs[idx];
    if (!ref.is_valid()) return;
    m_treeview_games.get_selection()->select(ref.get_path());
    on_play_clicked();
}

void MainWindow::configure_columns() {
    // Get all columns to configure them
    auto columns = m_treeview_games.get_columns();

    // Map each VIEW column (in append order) to its backing MODEL column. The view
    // omits the model's `status` column, so a view index is NOT the same as its
    // model column id : deriving the sort id from the loop index made "Type" and
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
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Save"), Gtk::RESPONSE_OK);
    
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
    Gtk::Dialog dialog(_("About FinalBurn Neo"), *this, true);
    dialog.set_default_size(540, 380);

    // Get FBNeo directory path
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    std::filesystem::path fbneo_path(fbneo_executable);
    std::string fbneo_dir = fbneo_path.parent_path().string();

    // Create content
    auto content_area = dialog.get_content_area();
    content_area->set_spacing(8);
    content_area->set_margin_start(16);
    content_area->set_margin_end(16);
    content_area->set_margin_top(10);
    content_area->set_margin_bottom(6);

    auto label = Gtk::manage(new Gtk::Label());
    std::string info_text = "<b>" + Glib::Markup::escape_text(_("FinalBurn Neo")) + "</b>\n";
    info_text += Glib::Markup::escape_text(_("A powerful arcade and console emulator")) + "\n\n";

    info_text += Glib::Markup::escape_text(_(
        "The launcher doesn't run the official Linux build: FinalBurn Neo's own team no "
        "longer maintains the SDL2/Linux port, which had quietly stopped generating game "
        "lists for some systems (Game Boy Advance, Bally Astrocade). We track their "
        "upstream repository daily and publish our own build with that fixed, so every "
        "system is covered."));
    info_text += "\n\n";

    info_text += "<b>" + Glib::Markup::escape_text(_("Configured executable:")) + "</b> "
               + Glib::Markup::escape_text(fbneo_executable.empty() ? _("(not set)") : fbneo_executable) + "\n";

    if (!fbneo_executable.empty()) {
        std::time_t mtime = get_file_mtime(fbneo_executable);
        if (mtime >= 0) {
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&mtime));
            info_text += "<b>" + Glib::Markup::escape_text(_("Installed build date:")) + "</b> " + buf + "\n";
        }
    }

    if (!m_fbneo_update_tag.empty()) {
        info_text += "<b>" + Glib::Markup::escape_text(_("Latest available fork build:")) + "</b> "
                   + Glib::Markup::escape_text(m_fbneo_update_tag);
        if (!m_fbneo_update_sha.empty())
            info_text += " (" + m_fbneo_update_sha.substr(0, 8) + ")";
        info_text += "\n";
    }

    label->set_markup(info_text);
    label->set_line_wrap(true);
    label->set_justify(Gtk::JUSTIFY_LEFT);
    content_area->pack_start(*label, Gtk::PACK_SHRINK);

    // Add buttons for documentation
    auto button_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 5));
    button_box->set_margin_top(14);

    // Our fork : the build the launcher actually downloads/runs.
    auto fork_button = Gtk::manage(new Gtk::Button(_("🔧 Our Linux fork (code)")));
    fork_button->signal_clicked().connect([this]() {
        spawn_process({"xdg-open", "https://github.com/battousai90/FBNeo"});
    });
    button_box->pack_start(*fork_button);

    // Official upstream project : for general documentation/credits.
    auto github_button = Gtk::manage(new Gtk::Button(_("🌐 Official FinalBurn Neo (upstream)")));
    github_button->signal_clicked().connect([this]() {
        spawn_process({"xdg-open", "https://github.com/finalburnneo/FBNeo"});
    });
    button_box->pack_start(*github_button);

    // Download the latest fork build directly from this dialog.
    auto download_button = Gtk::manage(new Gtk::Button(_("⬇ Download latest build")));
    download_button->signal_clicked().connect([this]() {
        on_download_latest_fbneo();
    });
    button_box->pack_start(*download_button);

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
    dialog.add_button(_("Close"), Gtk::RESPONSE_CLOSE);
    
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
#ifdef FBNEO_VERSION
    std::string about_text = "FBNeo Launcher " FBNEO_VERSION "\n\n";
#else
    std::string about_text = "FBNeo Launcher\n\n";
#endif
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

void MainWindow::check_fbneo_update_async() {
    std::thread([this]() {
        auto r = FbneoUpdateCheck::fetch_latest();
        if (r.ok) {
            m_fbneo_update_sha = r.sha;
            m_fbneo_update_tag = r.tag;
            m_fbneo_update_published_at = r.published_at;
        }
        m_fbneo_update_dispatcher.emit();
    }).detach();
}

// ── Online scores ──────────────────────────────────────────────────────
bool MainWindow::game_ranks_online(const std::string& system, const std::string& game) {
    std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
    return m_hiscore_supported.count(HiscoreClient::key(system, game)) > 0;
}

void MainWindow::refresh_hiscore_data_async(bool announce) {
    // L'interrupteur maître coupe tout : aucune pastille, aucune requête.
    if (!m_settings_panel.is_hiscore_enabled()) {
        std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
        m_hiscore_supported.clear();
        return;
    }
    // Un seul rafraîchissement à la fois : un joueur qui clique trois fois ne
    // doit pas déclencher trois chargements complets.
    if (m_hiscore_refreshing.exchange(true)) return;

    // The service address lives in config.json rather than being compiled in:
    // the same build has to serve a player pointed at the homelab, one pointed
    // at a public instance, and one pointed at nothing at all.
    std::string url;
    {
        nlohmann::json j;
        std::ifstream fi(AppContext::get_config_path());
        if (fi) { try { fi >> j; } catch (...) {} }
        // Defaults to the public service so a fresh install shows the
        // leaderboards without anyone having to configure anything.
        url = j.value("hiscore_url", std::string("https://scores.bootcade.duckdns.org"));
    }
    if (url.empty()) { m_hiscore_refreshing = false; return; }
    HiscoreClient::set_base_url(url);
    HiscoreClient::set_store_dir(
        std::filesystem::path(AppContext::get_config_path()).parent_path().string());

    // Dernière liste connue d'abord, pour que les pastilles soient justes dès
    // la première image et le restent sans réseau.
    {
        auto cached = HiscoreClient::cached_supported();
        if (!cached.empty()) {
            std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
            m_hiscore_supported = std::move(cached);
        }
    }

    if (announce) {
        std::lock_guard<std::mutex> lock(m_hiscore_status_mutex);
        m_hiscore_status = _("Refreshing highscores…");
        m_hiscore_refresh_dispatcher.emit();
    }

    std::thread([this, announce, alive = m_alive_token]() {
        int sent = HiscoreClient::flush_outbox();
        if (sent > 0) {
            std::lock_guard<std::mutex> live(alive->mutex);
            if (!alive->alive) return;
            std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
            m_hiscore_results.push_back(Glib::ustring::compose(
                _("%1 score(s) saved offline have now been sent."), sent).raw());
            m_hiscore_result_dispatcher.emit();
        }

        auto supported = HiscoreClient::fetch_supported();
        bool reached = !supported.empty();
        // Tous les classements en une fois. C'est ce qui permet à la
        // sélection d'un jeu de n'émettre aucune requête.
        auto boards = reached ? HiscoreClient::fetch_boards(10)
                              : std::vector<HiscoreClient::Board>();

        std::lock_guard<std::mutex> live(alive->mutex);
        if (!alive->alive) return;
        if (reached) {
            HiscoreClient::cache_supported(supported);
            if (!boards.empty()) HiscoreClient::cache_boards(boards);
            {
                std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
                m_hiscore_supported = std::move(supported);
            }
            m_hiscore_supported_dispatcher.emit();
        }
        m_hiscore_refreshing = false;
        if (announce) {
            std::lock_guard<std::mutex> lock(m_hiscore_status_mutex);
            m_hiscore_status = reached ? _("Highscores up to date.")
                                       : _("Score service unreachable.");
            m_hiscore_refresh_dispatcher.emit();
        }
    }).detach();
}

void MainWindow::on_hiscore_refresh_done() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_status_mutex);
        text = m_hiscore_status;
    }
    m_status_label.set_text(text);
}

void MainWindow::on_hiscore_supported_ready() {
    // The list lands after the games are already on screen, so the rows that
    // deserve a pill have to be revisited. Cheaper than delaying the whole
    // catalogue behind a network call the player did not ask for.
    for (auto& row : m_model_games->children()) {
        std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
        std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
        row[m_columns.m_col_hiscore] = game_ranks_online(system, name)
                                     ? Glib::ustring("\u25cf") : Glib::ustring();
    }
    // The filter tree carries a count of ranked games, and the Highscore sort
    // depends on the same list: both were built before it arrived.
    populate_filter_tree();
    if (m_sort_mode == SortMode::Highscore) filter_games();
    else rebuild_mlist();

    auto sel = m_treeview_games.get_selection();
    if (sel) { if (auto it = sel->get_selected()) show_game_details(*it); }
}

void MainWindow::fetch_hiscore_top_async(const std::string& system, const std::string& game) {
    unsigned seq;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
        seq = ++m_hiscore_seq;
    }
    // Paint whatever we already knew, straight away. Offline this is all the
    // player will get; online it removes the blank moment before the reply.
    {
        std::string when;
        auto cached = HiscoreClient::cached_top(system, game, &when);
        if (!cached.empty()) {
            std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
            m_hiscore_top = std::move(cached);
            m_hiscore_top_stale = when;
            m_hiscore_seq_done = seq;
        }
    }
    m_hiscore_top_dispatcher.emit();

    std::thread([this, system, game, seq, alive = m_alive_token]() {
        auto rows = HiscoreClient::fetch_top(system, game, 50);
        std::lock_guard<std::mutex> live(alive->mutex);
        if (!alive->alive) return;
        if (rows.empty() && !HiscoreClient::cached_top(system, game, nullptr).empty())
            return;                      // unreachable: keep what is on screen
        HiscoreClient::cache_top(system, game, rows);
        {
            std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
            // A reply for a game the player has already scrolled past must not
            // overwrite the one they are looking at now.
            if (seq != m_hiscore_seq) return;
            m_hiscore_top = std::move(rows);
            m_hiscore_top_stale.clear();
            m_hiscore_seq_done = seq;
        }
        m_hiscore_top_dispatcher.emit();
    }).detach();
}

void MainWindow::show_cached_board(const std::string& system, const std::string& game) {
    std::string when;
    auto rows = HiscoreClient::cached_top(system, game, &when);
    // La date n'est affichée que si le cache est vraiment vieux : au
    // démarrage il vient d'être rempli, la mentionner serait du bruit.
    bool old = false;
    if (!when.empty()) {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        if (strptime(when.c_str(), "%Y-%m-%dT%H:%M:%S", &tm))
            old = std::difftime(now, timegm(&tm)) > 3600;
    }
    render_board(rows, old ? when : std::string());
}

void MainWindow::render_board(const std::vector<HiscoreClient::Entry>& rows,
                              const std::string& stale) {
    std::string me;
    {
        nlohmann::json j;
        std::ifstream fi(AppContext::get_config_path());
        if (fi) { try { fi >> j; } catch (...) {} }
        me = j.value("hiscore_player", std::string());
    }

    for (auto* c : m_hiscore_grid.get_children()) m_hiscore_grid.remove(*c);

    int my_rank = 0;
    if (!me.empty())
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].player == me) { my_rank = (int)i + 1; break; }

    m_hiscore_title.set_markup(
        "<b>" + escape_markup(_("Highscore")) + "</b>" +
        (my_rank > 0 ? "  <span foreground=\"#41d08a\" size=\"small\">" +
                       escape_markup(Glib::ustring::compose(_("you are %1st"), my_rank)) +
                       "</span>"
                     : std::string()));

    // Dix lignes, toujours. Les places libres sont dessinees, pas ecrites en
    // gris : elles doivent se lire comme des places a prendre.
    const size_t BOARD_ROWS = 10;
    auto cell = [](const std::string& text, const char* css, float xalign) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(xalign);
        l->get_style_context()->add_class(css);
        return l;
    };
    for (size_t i = 0; i < BOARD_ROWS; ++i) {
        const bool free_slot = i >= rows.size();
        const bool mine = !free_slot && !me.empty() && rows[i].player == me;
        const char* tone = free_slot ? "hi-free" : (mine ? "hi-mine" : "hi-row");

        m_hiscore_grid.attach(*cell(std::to_string(i + 1), "hi-rank", 1.0f), 0, (int)i, 1, 1);
        m_hiscore_grid.attach(*cell(free_slot ? "." : format_score(rows[i].score),
                                    free_slot ? "hi-free" : (i == 0 ? "hi-score-top" : "hi-score"),
                                    1.0f), 1, (int)i, 1, 1);
        auto* who = cell(free_slot ? "AAA" : rows[i].player, tone, 0.0f);
        who->set_ellipsize(Pango::ELLIPSIZE_END);
        m_hiscore_grid.attach(*who, 2, (int)i, 1, 1);

        std::string right;
        if (!free_slot) {
            std::string flag = country_flag(rows[i].country);
            std::string day  = short_date(rows[i].since);
            right = flag + (flag.empty() || day.empty() ? "" : "  ") + day;
        }
        m_hiscore_grid.attach(*cell(right, "hi-when", 1.0f), 3, (int)i, 1, 1);
    }
    m_hiscore_grid.show_all();

    if (stale.empty()) {
        m_label_hiscore.hide();
    } else {
        m_label_hiscore.set_text(
            Glib::ustring::compose(_("offline, last seen %1"), short_date(stale)));
        m_label_hiscore.show();
    }
}

void MainWindow::on_hiscore_top_ready() {
    std::vector<HiscoreClient::Entry> rows;
    std::string stale;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
        if (m_hiscore_seq_done != m_hiscore_seq) return;
        rows = m_hiscore_top;
        stale = m_hiscore_top_stale;
    }
    render_board(rows, stale);
}

void MainWindow::sort_games(std::vector<Game>& games) {
    // std::stable_sort throughout: within equal keys the DAT order survives,
    // so sorting by year does not also shuffle the games of a given year.
    switch (m_sort_mode) {
    case SortMode::Default:
        break;                                   // DAT order, grouped by system
    case SortMode::Name:
        std::stable_sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
            // Compare the human title, which is what the views display; the
            // ROM name only breaks ties so the order stays deterministic.
            const std::string& ta = a.description.empty() ? a.name : a.description;
            const std::string& tb = b.description.empty() ? b.name : b.description;
            int c = g_utf8_collate(ta.c_str(), tb.c_str());
            return c != 0 ? c < 0 : a.name < b.name;
        });
        break;
    case SortMode::Year:
    case SortMode::YearAsc: {
        const bool ascending = (m_sort_mode == SortMode::YearAsc);
        std::stable_sort(games.begin(), games.end(), [ascending](const Game& a, const Game& b) {
            // Undated games sink to the bottom either way: they are the ones
            // the sort has nothing to say about, and floating them to the top
            // of "oldest first" would bury the answer the user asked for.
            bool ea = a.year.empty(), eb = b.year.empty();
            if (ea != eb) return !ea;
            if (ea) return false;
            return ascending ? a.year < b.year : a.year > b.year;
        });
        break;
    }
    case SortMode::RecentlyPlayed:
        std::stable_sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
            bool ea = a.last_played.empty(), eb = b.last_played.empty();
            if (ea != eb) return !ea;            // never played goes last
            if (ea) return false;
            return a.last_played > b.last_played;   // ISO-8601 sorts as text
        });
        break;
    case SortMode::Highscore: {
        // Copied once under the lock rather than consulted per comparison: a
        // sort over 29 000 games makes tens of thousands of comparisons.
        std::set<std::string> ranked;
        {
            std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
            ranked = m_hiscore_supported;
        }
        std::stable_sort(games.begin(), games.end(),
            [&ranked](const Game& a, const Game& b) {
                bool ra = ranked.count(HiscoreClient::key(a.system, a.name)) > 0;
                bool rb = ranked.count(HiscoreClient::key(b.system, b.name)) > 0;
                return ra != rb ? ra : false;
            });
        break;
    }
    }
}

void MainWindow::submit_session_score(const std::string& system,
                                      const std::string& game,
                                      const std::string& fbneo_rom_name,
                                      const std::string& hi_before,
                                      const std::string& player,
                                      const std::string& country) {
    // Each condition is one the player controls. None is an error worth
    // reporting: a game with no leaderboard, an unconfigured service or an
    // unticked box are all perfectly ordinary states.
    if (player.empty()) return;
    if (HiscoreClient::base_url().empty()) return;

    // Playtime is reported for ANY game, ranked or not. A clone, a hack or a
    // console port has no leaderboard : their scoring may differ : but the
    // player still spent that time, and dropping it made whole evenings vanish
    // from the record for no reason they could see.
    // Read after watch_playtime has written this session in, so the figures
    // sent are the ones just earned rather than the previous ones.
    HiscoreClient::Playtime pt;
    {
        Game g = m_database->getGame(game, system);
        pt.last    = g.last_session_secs;
        pt.longest = g.longest_session_secs;
        pt.total   = g.play_time_secs;
    }
    if (!game_ranks_online(system, game)) {
        if (pt.total > 0)
            HiscoreClient::submit(system, game, player, country, pt, "", "");
        return;
    }

    std::string hi_after = read_file_bytes(fbneo_hiscore_path(fbneo_rom_name));
    if (hi_after.empty()) return;          // game never wrote a score table
    if (hi_after == hi_before) return;     // nothing happened worth sending

    // FIRST SESSION ON THIS GAME : no table existed beforehand, so nothing in
    // this one is attributable. A score table ships full of factory entries
    // (Out Run starts at 5 000 000) and there is no way to tell them from a
    // player's row without a before state to compare against.
    //
    // Sending it anyway would put every new game through manual review, and
    // hand the administrator a factory number as the only clue : noise for
    // them, and a wait for nothing for the player. So this session becomes
    // the reference instead, and every later one publishes on its own.
    //
    // The cost is the very first score on a brand-new game. That is the right
    // trade: it happens once per game, and the alternative asks a human to
    // adjudicate something nobody has the information to adjudicate.
    if (hi_before.empty()) {
        // The score is not attributable, but the session still happened: the
        // playtime goes up on its own so a first game is not missing from the
        // record entirely.
        if (pt.total > 0)
            HiscoreClient::submit(system, game, player, country, pt, "", "");
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back(
            _("First run on this game : saved as the reference. "
              "Your next score will be sent automatically."));
        m_hiscore_result_dispatcher.emit();
        return;
    }

    auto r = HiscoreClient::submit(system, game, player, country, pt,
                                   hi_before, hi_after);

    // An unreachable service is deliberately silent. The player did not ask
    // to publish anything at that instant, and a network error popping up
    // after every offline session would be pure noise.
    if (!r.reached) {
        // Parked, not dropped. FBNeo overwrites the .hi on the next session,
        // so this is the last moment the evidence still exists.
        HiscoreClient::queue_submission(system, game, player, country, pt,
                                        hi_before, hi_after);
        std::cout << "[HISCORE] queued for later (" << r.error << ")" << std::endl;
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back(
            _("Server unreachable : your score is saved and will be sent later."));
        m_hiscore_result_dispatcher.emit();
        return;
    }

    // A run that beat nothing is the ordinary case, not an event. Announcing
    // "score not kept" after every session would turn the notification into
    // noise and teach the player to ignore it.
    if (r.ignored) return;

    std::string message;
    if (r.accepted) {
        message = r.has_score
            ? Glib::ustring::compose(_("Score %1 published on the leaderboard."),
                                     format_score(r.score)).raw()
            : _("Score published on the leaderboard.");
    } else if (r.pending) {
        // Said as a wait, not a suspicion: the usual causes are a first score
        // on a game or a missing baseline, neither of which is the player's
        // fault, and "rejected" would read as an accusation.
        message = r.has_score
            ? Glib::ustring::compose(_("Score %1 sent : awaiting review."),
                                     format_score(r.score)).raw()
            : _("Score sent : awaiting review.");
        // The server's reason is an administrator's diagnostic, written in the
        // server's language and in its vocabulary. Pasting it here produced a
        // half-translated sentence about baselines and table heads, which says
        // nothing to a player who just wants to know if their score counted.
        // It goes to the log, where it belongs.
        if (!r.reason.empty())
            std::cout << "[HISCORE] queued: " << r.reason << std::endl;
    } else {
        // A refusal with an explanation, e.g. a clone having no leaderboard.
        message = r.reason.empty() ? _("Score not kept.") : r.reason;
    }

    {
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back(message);
    }
    m_hiscore_result_dispatcher.emit();
}

void MainWindow::on_hiscore_result_ready() {
    std::string message;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        if (m_hiscore_results.empty()) return;
        message = m_hiscore_results.front();
        m_hiscore_results.pop_front();
    }
    m_hiscore_infobar_label.set_text(message);
    m_hiscore_infobar.show();

    // The leaderboard on screen is now out of date if it is the game just
    // played : refresh whatever the detail dock is showing.
    auto sel = m_treeview_games.get_selection();
    if (sel) { if (auto it = sel->get_selected()) show_game_details(*it); }
}

void MainWindow::on_fbneo_update_check_result() {
    if (m_fbneo_update_sha.empty()) return; // fetch failed : stay silent, not an error the user needs to see

    nlohmann::json j;
    { std::ifstream fi(AppContext::get_config_path()); if (fi) { try { fi >> j; } catch (...) {} } }
    std::string known_sha = j.value("fbneo_release_sha", "");

    if (!known_sha.empty()) {
        // We have a launcher-recorded baseline (a download done through this
        // app) : the SHA comparison is exact, use it.
        if (known_sha != m_fbneo_update_sha) {
            m_fbneo_update_label.set_text(_("A new FBNeo version is available."));
            m_fbneo_update_infobar.show();
        }
        return;
    }

    // No baseline recorded: the configured executable was never downloaded
    // through this launcher (e.g. built by hand, or dropped in manually), so
    // there is no SHA to compare against. Fall back to comparing the
    // executable's mtime against the release's publish date : approximate,
    // but better than staying silent forever for these users.
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    if (fbneo_executable.empty()) return;

    std::time_t exe_mtime = get_file_mtime(fbneo_executable);
    if (exe_mtime < 0) return;

    std::time_t published = FbneoUpdateCheck::parse_iso8601(m_fbneo_update_published_at);
    if (published < 0) return; // can't tell, don't guess

    if (exe_mtime < published) {
        m_fbneo_update_label.set_text(_("A new FBNeo version is available."));
        m_fbneo_update_infobar.show();
    }
}

void MainWindow::on_fbneo_update_infobar_response(int response_id) {
    if (response_id == Gtk::RESPONSE_OK) {
        on_download_latest_fbneo(); // hides the infobar itself once the dialog opens
    } else {
        m_fbneo_update_infobar.hide();
    }
}

void MainWindow::on_download_latest_fbneo() {
    m_fbneo_update_infobar.hide();

    // $HOME, not current_path(): the working directory a launch happens to start
    // in is not stable across a desktop icon vs. a terminal vs. a dev checkout,
    // so this could silently extract into a different folder each time.
    const char* home_env = std::getenv("HOME");
    auto download_dialog = std::make_unique<DownloadDialog>(
        *this,
        // See SettingsPanel::on_download_fbneo_clicked for why this points at our
        // own fork instead of finalburnneo/FBNeo directly.
        "https://github.com/battousai90/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        home_env ? std::string(home_env) : std::filesystem::current_path().string()
    );

    download_dialog->set_settings_entry(&m_settings_panel.m_entry_fbneo);
    download_dialog->start_download();
    int result = download_dialog->run();

    // Record what "latest" pointed at just now, so a future startup check has a
    // baseline to compare against. The startup check already did this fetch in
    // most cases (m_fbneo_update_sha), so this usually costs nothing extra;
    // it only falls back to a fresh call if that never completed.
    std::string sha = m_fbneo_update_sha;
    if (sha.empty()) {
        auto r = FbneoUpdateCheck::fetch_latest();
        if (r.ok) sha = r.sha;
    }
    if (!sha.empty()) {
        nlohmann::json j;
        const std::string path = AppContext::get_config_path();
        { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
        j["fbneo_release_sha"] = sha;
        std::ofstream fo(path);
        if (fo) fo << j.dump(4);
    }

    if (result != Gtk::RESPONSE_OK) return; // download failed or was cancelled

    // A new build usually means new/changed game definitions : offer to chain
    // straight into DAT generation and the database update so the user ends
    // up with a working, up-to-date library in one flow instead of having to
    // remember these two extra steps.
    ConfirmationDialog confirm_dialog(*this,
        _("Generate DAT files?"),
        _("Generate DAT files from the new FBNeo build and update the game database now?"),
        "⚙️");
    if (!confirm_dialog.show_and_confirm()) return;

    GenerateDAT::execute(*this, m_settings_panel.get_fbneo_executable(), m_settings_panel.get_dat_path());
    // Not on_update_dat_clicked(): that shows its own "continue?" confirmation,
    // which : coming right after this dialog's own confirm and GenerateDAT's
    // own success dialog : is an easy dialog to reflexively dismiss. Cancelling
    // it silently skipped the database reload entirely: the DAT files on disk
    // were current, but the games table (and the audit reading it) stayed on
    // the old snapshot with no error or indication anything was wrong.
    do_update_dat();
}

void MainWindow::on_generate_dat_files() {
    GenerateDAT::execute(*this, m_settings_panel.get_fbneo_executable(), m_settings_panel.get_dat_path());
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
    m_status_label.set_text(_("Download cancelled by user"));
}

void MainWindow::start_scan_thread(const std::vector<std::string>& roms_paths) {
    if (m_scan_in_progress) return; // prevent double-launch

    m_scan_in_progress = true;
    m_scan_cancelled   = false;
    m_button_scan.set_sensitive(false);

    // Show inline scan progress in the status bar immediately.
    // show_all() is needed because set_no_show_all(true) was set at construction
    // (to prevent the box from appearing during the initial window show_all call).
    m_scan_progress_label.set_text(_("🔍 Initializing scan…"));
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

    // Create dialog on the heap (non-modal) : destroyed when user closes it
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
    std::cout << "[INFO] Scan complete : refreshing game list" << std::endl;

    m_scan_bg_poll_timer.disconnect();

    m_cached_games = m_database->getAllGames();

    // Regenerate the filter cache so the left panel shows updated counts/systems
    m_filter_cache = FilterCache::generate_from_games(m_cached_games);
    save_filter_cache();
    m_filter_cache_loaded = true;
    populate_filter_tree();

    filter_games();
    update_status_bar_stats();

    // ROM Management's own views (Outbox/Quarantine/Library audit) are stale
    // the moment any scan finishes : most directly the one "Move to library"
    // itself triggers, closing the loop back to "the set now shows fixed".
    if (m_rom_manager) m_rom_manager->refresh_after_scan();

    m_scan_in_progress = false;
    m_button_scan.set_sensitive(true);

    // Replace progress bar with a brief "done" (or "cancelled") message then hide after 4 s
    size_t avail = m_database->getGameCountByStatus("available");
    bool was_cancelled = m_scan_dialog && m_scan_dialog->was_cancelled();
    if (was_cancelled) {
        m_scan_progress_label.set_text(Glib::ustring::compose(
            _("🛑 Scan cancelled : %1 games available"), avail));
    } else {
        m_scan_progress_label.set_text(Glib::ustring::compose(
            _("✅ Scan complete : %1 games available"), avail));
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
    // Dialog hid itself : polling was already running, nothing extra needed.
    // "📊 Details" button remains visible and reopens the dialog on click.
}

bool MainWindow::on_scan_bg_poll() {
    if (!m_scan_dialog) return false; // dialog destroyed : stop timer

    double pct = m_scan_dialog->get_scan_progress();
    std::string msg = m_scan_dialog->get_scan_message();

    // Shorten message for status bar (strip leading emoji + long paths)
    if (msg.size() > 45) msg = msg.substr(0, 42) + "…";

    int p = static_cast<int>(pct);
    m_scan_progress_label.set_text(Glib::ustring::compose(_("🔍 %1%%  %2"), p, msg));
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
    // Guard the whole rebuild: clearing the model drags the selection across the
    // remaining rows and fires selection-changed for each of them. The ★ Favorites
    // row is one of those, so a plain rescan used to silently switch the library
    // to favourites-only. Scoped so the early return below cannot leave it set.
    struct Guard {
        bool& flag;
        explicit Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(m_populating_filters);

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
    std::unordered_map<std::string, int> type_counts;
    int favorite_count = 0;

    for (const auto& game : m_cached_games) {
        // Release type : a set can match several (a hack is usually a clone too).
        if (game.is_original())  type_counts["original"]++;
        if (game.is_clone())     type_counts["clone"]++;
        if (game.is_hack())      type_counts["hack"]++;
        if (game.is_homebrew())  type_counts["homebrew"]++;
        if (game.is_bootleg())   type_counts["bootleg"]++;
        if (game.is_prototype()) type_counts["prototype"]++;
        if (game.is_favorite)    favorite_count++;

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

    // Favourites : a top-level entry mirroring the header star toggle.
    {
        auto fav = m_model_filters->append();
        (*fav)[m_filter_columns.m_col_icon] = get_filter_icon("Favorites");
        (*fav)[m_filter_columns.m_col_name] = std::string("★ ") + _("Favorites")
                                            + " (" + std::to_string(favorite_count) + ")";
        (*fav)[m_filter_columns.m_col_type] = "favorite";
        (*fav)[m_filter_columns.m_col_value] = "1";
        (*fav)[m_filter_columns.m_col_count] = favorite_count;
    }

    // Games the score service can rank. Counted here rather than kept as a
    // running total because the supported list can land after the tree is
    // first built, and the node has to show a truthful number either way.
    {
        int ranked_count = 0;
        for (const auto& game : m_cached_games)
            if (game_ranks_online(game.system, game.name)) ranked_count++;
        if (ranked_count > 0) {
            auto hi = m_model_filters->append();
            (*hi)[m_filter_columns.m_col_icon] = get_filter_icon("Highscore");
            (*hi)[m_filter_columns.m_col_name] = std::string("◆ ") + _("Highscore")
                                               + " (" + std::to_string(ranked_count) + ")";
            (*hi)[m_filter_columns.m_col_type] = "hiscore";
            (*hi)[m_filter_columns.m_col_value] = "1";
            (*hi)[m_filter_columns.m_col_count] = ranked_count;
        }
    }

    // Release type: originals vs the derivative sets (clones, hacks, …).
    {
        auto type_root = m_model_filters->append();
        (*type_root)[m_filter_columns.m_col_icon] = get_filter_icon("Type");
        (*type_root)[m_filter_columns.m_col_name] = _("Type");
        (*type_root)[m_filter_columns.m_col_type] = "category";
        (*type_root)[m_filter_columns.m_col_value] = "";

        const std::vector<std::pair<std::string, std::string>> types = {
            {"original",  _("Original")},  {"clone",     _("Clone")},
            {"hack",      _("Hack")},      {"homebrew",  _("Homebrew")},
            {"bootleg",   _("Bootleg")},   {"prototype", _("Prototype")}};
        for (const auto& [key, label] : types) {
            int count = type_counts[key];
            if (count == 0) continue;
            auto child = m_model_filters->append(type_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = label + " (" + std::to_string(count) + ")";
            (*child)[m_filter_columns.m_col_type] = "type";
            (*child)[m_filter_columns.m_col_value] = key;
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

    // Coloured status dots (green/amber/grey) instead of a plain checkmark.
    auto status_dot = [](const char* color) -> Glib::RefPtr<Gdk::Pixbuf> {
        std::string svg = "<svg width='14' height='14' xmlns='http://www.w3.org/2000/svg'>"
                          "<circle cx='7' cy='7' r='5' fill='" + std::string(color) + "'/></svg>";
        try {
            auto loader = Gdk::PixbufLoader::create("svg");
            loader->set_size(14, 14);
            loader->write(reinterpret_cast<const guint8*>(svg.data()), svg.size());
            loader->close();
            return loader->get_pixbuf();
        } catch (...) { return {}; }
    };
    for (const auto& [status, count] : status_counts) {
        const char* color = status == "available" ? "#41d08a"
                          : status == "incorrect" ? "#f0b54a"
                          : status == "missing"   ? "#5a6272" : "#939aab";
        auto child = m_model_filters->append(status_root->children());
        (*child)[m_filter_columns.m_col_icon] = status_dot(color);
        (*child)[m_filter_columns.m_col_name] = status + " (" + std::to_string(count) + ")";
        (*child)[m_filter_columns.m_col_type] = "status";
        (*child)[m_filter_columns.m_col_value] = status;
        (*child)[m_filter_columns.m_col_count] = count;
    }

    m_treeview_filters.collapse_all();
    std::cout << "[INFO] Filter tree populated successfully" << std::endl;
}

void MainWindow::on_filter_selection_changed() {
    // Selection changes emitted while the tree is being rebuilt are GTK bookkeeping,
    // not user intent.
    if (m_populating_filters) return;

    auto selection = m_treeview_filters.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;
    
    Gtk::TreeModel::Row row = *iter;
    Glib::ustring type_ustring = row[m_filter_columns.m_col_type];
    Glib::ustring value_ustring = row[m_filter_columns.m_col_value];
    std::string type = type_ustring.raw();
    std::string value = value_ustring.raw();
    
    if (type == "root") {
        // "All Games" is the only reset: drop every dimension and the star.
        m_active_filters.clear();
        m_show_favorites_only = false;
        m_suppress_fav_toggle = true;
        m_btn_favorites.set_active(false);
        m_suppress_fav_toggle = false;
    } else if (type == "category") {
        // A category row is just an expander. It used to clear every filter,
        // which made stacking impossible: reaching Type > Original meant
        // clicking "Type" first, which silently dropped the system filter.
        return;
    } else if (type == "favorite") {
        // Mirrors the header star; leaves the other dimensions alone.
        m_show_favorites_only = true;
        m_suppress_fav_toggle = true;
        m_btn_favorites.set_active(true);
        m_suppress_fav_toggle = false;
    } else {
        // One value per dimension: picking another system replaces the system,
        // but leaves the type/year/… filters standing.
        m_active_filters[type] = value;
    }

    apply_tree_filters();
}

void MainWindow::apply_tree_filters() {
    rebuild_filter_chips();

    // Detach the model and disable sort during the bulk rebuild : GTK
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
            if (filter_type == "type") {
                bool ok = filter_value == "original"  ? game.is_original()
                        : filter_value == "clone"     ? game.is_clone()
                        : filter_value == "hack"      ? game.is_hack()
                        : filter_value == "homebrew"  ? game.is_homebrew()
                        : filter_value == "bootleg"   ? game.is_bootleg()
                        : filter_value == "prototype" ? game.is_prototype()
                        : true;
                if (!ok) { matches = false; break; }
            }
            if (filter_type == "favorite" && !game.is_favorite) {
                matches = false; break;
            }
            if (filter_type == "hiscore" && !game_ranks_online(game.system, game.name)) {
                matches = false; break;
            }
        }

        if (!matches) continue;

        // Header star toggle: an overlay on top of whichever filter is active,
        // so "Favorites + Neo Geo" narrows rather than replaces.
        if (m_show_favorites_only && !game.is_favorite) continue;
        
        // Apply search filter
        if (!search_text.empty()) {
            std::string game_name  = game.name;
            std::string game_desc  = game.description;
            std::string game_manuf = game.manufacturer;
            std::string game_year  = game.year;
            std::transform(game_name.begin(),  game_name.end(),  game_name.begin(),  ::tolower);
            std::transform(game_desc.begin(),  game_desc.end(),  game_desc.begin(),  ::tolower);
            std::transform(game_manuf.begin(), game_manuf.end(), game_manuf.begin(), ::tolower);
            // year is numeric : compare as-is (search_text already lowered, no-op for digits)

            if (game_name.find(search_text)  == std::string::npos &&
                game_desc.find(search_text)  == std::string::npos &&
                game_manuf.find(search_text) == std::string::npos &&
                game_year.find(search_text)  == std::string::npos) {
                continue;
            }
        }
        
        filtered_games.push_back(game);
    }
    
    sort_games(filtered_games);

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
        row[m_columns.m_col_hiscore] = game_ranks_online(game.system, game.name)
                                     ? Glib::ustring("\u25cf") : Glib::ustring();
        row[m_columns.m_col_last_played] = game.last_played;
    }
    
    // Restore sort + reattach model (single redraw instead of one per insert)
    if (had_sort &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID) {
        m_model_games->set_sort_column(prev_sort_col, prev_sort_order);
    }
    m_treeview_games.set_model(m_model_games);

    // Rebuild the active custom view so its row references stay valid.
    refresh_active_view();

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
    // Cache pixbufs per category : called once per filter row and the "item"
    // bucket is hit dozens of times per filter-tree rebuild.
    static std::unordered_map<std::string, Glib::RefPtr<Gdk::Pixbuf>> cache;
    auto it = cache.find(category);
    if (it != cache.end()) return it->second;

    // Clean, consistent line icons drawn as SVG (accent for categories, muted for
    // items) so the sidebar matches the modern theme instead of mismatched bitmaps.
    const std::string accent = "#9a8cff";
    const std::string muted  = "#9aa0b0";
    std::string color = accent;
    std::string body;
    if (category == "All Games") {
        body = "<rect x='2' y='3' width='12' height='4' rx='1'/><rect x='2' y='9' width='12' height='4' rx='1'/>";
    } else if (category == "Systems") {
        body = "<rect x='2' y='3' width='12' height='8' rx='1'/><path d='M6 13h4M8 11v2'/>";
    } else if (category == "Manufacturers") {
        body = "<rect x='3' y='2' width='10' height='12' rx='1'/><path d='M6 5h1M9 5h1M6 8h1M9 8h1M7 14v-3h2v3'/>";
    } else if (category == "Years") {
        body = "<rect x='2' y='3' width='12' height='11' rx='1.5'/><path d='M2 6h12M5 2v3M11 2v3'/>";
    } else if (category == "Sources") {
        body = "<path d='M4 2h5l4 4v8a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z'/><path d='M9 2v4h4'/>";
    } else if (category == "Aspect Ratio") {
        body = "<rect x='2' y='4' width='12' height='8' rx='1'/>";
    } else if (category == "Orientation") {
        body = "<path d='M12 8a4 4 0 1 1-1.5-3.1'/><path d='M12 3v3h-3'/>";
    } else if (category == "ROM Status") {
        body = "<circle cx='8' cy='8' r='6'/><path d='M8 7v4'/><circle cx='8' cy='5' r='0.7' fill='" + accent + "' stroke='none'/>";
    } else if (category == "Favorites") {
        body = "<path d='M8 2l1.9 3.8 4.1.6-3 2.9.7 4.1L8 11.5 4.3 13.4l.7-4.1-3-2.9 4.1-.6z'/>";
    } else if (category == "Highscore") {
        body = "<path d='M8 2l4 6-4 6-4-6z'/>";
    } else if (category == "Type") {
        body = "<path d='M8 2l5 3v6l-5 3-5-3V5z'/><path d='M8 8l5-3M8 8v6M8 8L3 5'/>";
    } else { // leaf item
        color = muted;
        body = "<path d='M8 3.5 13 8 8 12.5 3 8Z'/>";
    }

    std::string svg =
        "<svg width='16' height='16' viewBox='0 0 16 16' xmlns='http://www.w3.org/2000/svg' "
        "fill='none' stroke='" + color + "' stroke-width='1.3' stroke-linejoin='round' stroke-linecap='round'>"
        + body + "</svg>";

    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    try {
        auto loader = Gdk::PixbufLoader::create("svg");
        loader->set_size(18, 18);
        loader->write(reinterpret_cast<const guint8*>(svg.data()), svg.size());
        loader->close();
        pixbuf = loader->get_pixbuf();
    } catch (...) {
        pixbuf = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 16, 16);
    }

    cache.emplace(category, pixbuf);
    return pixbuf;
}

// ── Launch preference persistence ─────────────────────────────────────────

void MainWindow::rebuild_filter_chips() {
    for (auto* c : m_chips_box.get_children()) m_chips_box.remove(*c);

    // Human labels for the dimension keys stored in m_active_filters.
    auto dim_label = [](const std::string& k) -> std::string {
        if (k == "system")       return _("System");
        if (k == "manufacturer") return _("Manufacturer");
        if (k == "year")         return _("Year");
        if (k == "source")       return _("Source");
        if (k == "aspect")       return _("Aspect");
        if (k == "orientation")  return _("Orientation");
        if (k == "status")       return _("Status");
        if (k == "type")         return _("Type");
        if (k == "mode")         return _("Mode");
        if (k == "hiscore")      return _("Highscore");
        return k;
    };
    auto value_label = [](const std::string& k, const std::string& v) -> std::string {
        if (k != "type") return v;
        if (v == "original")  return _("Original");
        if (v == "clone")     return _("Clone");
        if (v == "hack")      return _("Hack");
        if (v == "homebrew")  return _("Homebrew");
        if (v == "bootleg")   return _("Bootleg");
        if (v == "prototype") return _("Prototype");
        return v;
    };

    // One chip per active dimension, plus the star as its own chip.
    auto add_chip = [&](const std::string& text, std::function<void()> on_remove) {
        auto* chip = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 4);
        chip->get_style_context()->add_class("filter-chip");
        auto* lbl = Gtk::make_managed<Gtk::Label>(text);
        auto* x = Gtk::make_managed<Gtk::Button>();
        x->set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_MENU);
        x->set_relief(Gtk::RELIEF_NONE);
        x->get_style_context()->add_class("chip-x");
        x->signal_clicked().connect([this, on_remove] {
            on_remove();
            apply_tree_filters();
        });
        chip->pack_start(*lbl, Gtk::PACK_SHRINK);
        chip->pack_start(*x, Gtk::PACK_SHRINK);
        m_chips_box.pack_start(*chip, Gtk::PACK_SHRINK);
    };

    if (m_show_favorites_only) {
        add_chip(std::string("★ ") + _("Favorites"), [this] {
            m_show_favorites_only = false;
            m_suppress_fav_toggle = true;
            m_btn_favorites.set_active(false);
            m_suppress_fav_toggle = false;
        });
    }
    for (const auto& [k, v] : m_active_filters) {
        add_chip(dim_label(k) + ": " + value_label(k, v),
                 [this, k] { m_active_filters.erase(k); });
    }

    // "Clear all" only earns its place once something is actually filtering.
    if (!m_chips_box.get_children().empty()) {
        auto* clear = Gtk::make_managed<Gtk::Button>(_("Clear all"));
        clear->set_relief(Gtk::RELIEF_NONE);
        clear->get_style_context()->add_class("chip-clear");
        clear->signal_clicked().connect([this] {
            m_active_filters.clear();
            m_show_favorites_only = false;
            m_suppress_fav_toggle = true;
            m_btn_favorites.set_active(false);
            m_suppress_fav_toggle = false;
            m_treeview_filters.get_selection()->unselect_all();
            apply_tree_filters();
        });
        m_chips_box.pack_start(*clear, Gtk::PACK_SHRINK);
    }

    m_chips_box.show_all();
    m_chips_box.set_visible(!m_chips_box.get_children().empty());
}

void MainWindow::set_favorites_only(bool on) {
    if (m_show_favorites_only == on) return;
    m_show_favorites_only = on;

    m_suppress_fav_toggle = true;
    m_btn_favorites.set_active(on);
    m_suppress_fav_toggle = false;

    apply_tree_filters();
}

void MainWindow::on_language_selected(const std::string& code) {
    // Persist the choice and prompt for a restart (labels are built once at
    // startup).
    m_settings_panel.set_language(code); // suppressed inside SettingsPanel
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
        if (j.contains("detail_dock_position")) {
            std::string p = j["detail_dock_position"].get<std::string>();
            m_dock_position = (p == "right") ? "right" : "bottom";
        }
        if (j.contains("grid_columns")) {
            int n = j["grid_columns"].get<int>();
            m_grid_columns = (n < 3) ? 3 : (n > 5) ? 5 : n;
        }
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
    j["detail_dock_position"] = m_dock_position;
    j["grid_columns"] = m_grid_columns;
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

void MainWindow::on_rom_manager() {
    if (!m_rom_manager) {
        m_rom_manager = std::make_unique<RomManagerWindow>(*this, m_database);

        // The DAT directory is the one setting both UIs own, so mirror it back
        // into the Settings panel and persist it there too.
        m_rom_manager->signal_dat_path_changed().connect([this](std::string path) {
            m_settings_panel.set_dat_path(path);
            m_settings_panel.save_to_file(AppContext::get_config_path());
        });

        m_rom_manager->signal_update_dat().connect(
            sigc::mem_fun(*this, &MainWindow::on_update_dat_clicked));

        // "Move to library" already moved files straight into existing ROM
        // directories : nothing to add, just verify the result with a scan.
        m_rom_manager->signal_scan_requested().connect([this] {
            start_scan_thread(m_settings_panel.get_roms_paths());
        });
        m_rom_manager->signal_rescan_requested().connect(
            sigc::mem_fun(*this, &MainWindow::on_start_scan_clicked));
    }

    // Pick up any path the Settings dialog changed while the window was closed.
    m_rom_manager->reload_settings();
    m_rom_manager->show();
    m_rom_manager->present();
}

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
    dlg.add_button(_("Close"), Gtk::RESPONSE_CLOSE);
    dlg.show_all_children();
    dlg.run();
}
