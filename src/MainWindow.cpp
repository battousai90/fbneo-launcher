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
    m_menu_item_quit.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_hide));
    m_submenu_file.append(m_menu_item_quit);

    // === Toolbar ===
    m_toolbar.set_spacing(10);
    m_button_scan.set_image_from_icon_name("system-search-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_scan.set_always_show_image(true);
    m_toolbar.pack_start(m_button_scan, Gtk::PACK_SHRINK);
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_scan_clicked));

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
    m_button_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_scan_clicked));

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
    if (m_scan_in_progress) {
        m_scan_cancel_requested = true;
        return;
    }

    m_scan_in_progress = true;
    m_scan_cancel_requested = false;

    m_button_scan.set_label("Cancel Scan");
    m_button_scan.set_image_from_icon_name("process-stop-symbolic", Gtk::ICON_SIZE_BUTTON);

    std::vector<Game> previous_games = m_cached_games;
    m_model_games->clear();
    m_cached_games.clear();

    std::string dat_path = m_settings_panel.get_dat_path();
    if (dat_path.empty()) {
        m_status_label.set_text("Error: DAT path not defined");
        m_status_label.show();
        m_scan_in_progress = false;
        m_button_scan.set_label("Scan ROMs");
        m_button_scan.set_image_from_icon_name("system-search-symbolic", Gtk::ICON_SIZE_BUTTON);
        return;
    }

    auto games = DatParser::parse(dat_path);
    if (games.empty()) {
        m_status_label.set_text("Error: No games loaded from DAT file");
        m_status_label.show();
        m_scan_in_progress = false;
        m_button_scan.set_label("Scan ROMs");
        m_button_scan.set_image_from_icon_name("system-search-symbolic", Gtk::ICON_SIZE_BUTTON);
        return;
    }

    std::string roms_path = m_settings_panel.get_roms_path();
    int total = games.size();
    bool scan_completed = false;

    for (size_t i = 0; i < games.size(); ++i) {
        if (m_scan_cancel_requested) {
            break;
        }

        Game mutable_game = games[i];
        RomScanner::check_availability(mutable_game, roms_path);
        m_cached_games.push_back(mutable_game);

        auto row = *m_model_games->append();
        row[m_columns.m_col_icon] = IconManager::get_status_icon(mutable_game.status);
        row[m_columns.m_col_name] = mutable_game.name;
        row[m_columns.m_col_title] = mutable_game.description;
        row[m_columns.m_col_year] = mutable_game.year;
        row[m_columns.m_col_manufacturer] = mutable_game.manufacturer;

        if (i % 10 == 0) {
            update_status_bar_scanning(i + 1, total, mutable_game.name);
            while (Gtk::Main::events_pending()) Gtk::Main::iteration();
        }
    }

    if (!m_scan_cancel_requested) {
        if (ScanCache::save(m_cached_games, AppContext::get_cache_path())) {
        }
        update_status_bar_stats();
        m_status_label.hide();
        scan_completed = true;
    }

    m_scan_in_progress = false;
    m_button_scan.set_label("Scan ROMs");
    m_button_scan.set_image_from_icon_name("system-search-symbolic", Gtk::ICON_SIZE_BUTTON);

    if (m_scan_cancel_requested) {
        m_model_games->clear();
        m_cached_games = previous_games;
        for (const auto& game : m_cached_games) {
            auto row = *m_model_games->append();
            row[m_columns.m_col_icon] = IconManager::get_status_icon(game.status);
            row[m_columns.m_col_name] = game.name;
            row[m_columns.m_col_title] = game.description;
            row[m_columns.m_col_year] = game.year;
            row[m_columns.m_col_manufacturer] = game.manufacturer;
        }
        update_status_bar_stats();
        m_status_label.hide();
    }
}

void MainWindow::on_cancel_scan_clicked() {
    m_scan_in_progress = false;
    m_scan_cancel_requested = true;

    m_button_scan.set_label("Scan ROMs");
    m_button_scan.set_image_from_icon_name("system-search-symbolic", Gtk::ICON_SIZE_BUTTON);
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

void MainWindow::update_status_bar_scanning(int current, int total, std::string filename) {
    int percent = (current * 100) / total;
    std::ostringstream oss;
    oss << "Scanning... " << filename << " (" << current << "/" << total << ", " << percent << "%)";
    m_status_label.set_text(oss.str());
    m_status_label.show();
}