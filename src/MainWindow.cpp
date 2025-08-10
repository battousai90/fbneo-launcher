// src/MainWindow.cpp
#include "MainWindow.h"
#include <iostream>
#include "GameRow.h"
#include "DatParser.h"
#include "RomScanner.cpp"
#include "SettingsPanel.h"
#include "Game.h"
#include "ModelColumns.h"

MainWindow::MainWindow() {
    set_title("fbneo-launcher");
    set_default_size(1000, 600);
    set_border_width(8);

    // loading configuration file
    m_settings_panel.load_from_file("config.json");


    // --- Toolbar ---
    m_toolbar.set_spacing(10);
    m_toolbar.pack_start(m_button_refresh, Gtk::PACK_SHRINK);
    m_search_entry.set_placeholder_text("Search game...");
    m_toolbar.pack_start(m_search_entry);

    // --- Tabs ---
    m_notebook.append_page(m_listbox_games, "Games");
    m_notebook.append_page(m_settings_panel, "Settings");
    m_main_box.pack_start(m_notebook);

    // --- TreeView Setup ---
    m_model_games = Gtk::ListStore::create(m_columns);
    m_treeview_games.set_model(m_model_games);

    // --- TreeView Columns ---
    m_treeview_games.append_column("Icon", m_columns.m_col_icon);
    m_treeview_games.append_column("Name", m_columns.m_col_name);
    m_treeview_games.append_column("Title", m_columns.m_col_title);
    m_treeview_games.append_column("Year", m_columns.m_col_year);
    m_treeview_games.append_column("Manufacturer", m_columns.m_col_manufacturer);

    // Connect the selection changed signal to handle game selection
    m_treeview_games.get_selection()->signal_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_game_selected)
    );

    // --- ListBox for games ---
    m_scrolled_games.add(m_treeview_games);

    // --- details box ---
    m_preview_image.set_size_request(200, 150);
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

    // Center the details box
    m_details_box.set_halign(Gtk::ALIGN_CENTER);
    m_details_box.set_valign(Gtk::ALIGN_START);

    // --- Paned container ---
    m_paned.pack1(m_scrolled_games, true, false);           // Gauche : liste
    m_paned.pack2(m_details_box, false, true);              // Droite : détails

    // --- Final assembly ---
    m_main_box.pack_start(m_toolbar, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_paned);

    add(m_main_box);

    m_button_refresh.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_refresh_clicked));
    m_button_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));

    populate_from_dat();

    show_all_children();
}

MainWindow::~MainWindow() {}

Glib::RefPtr<Gdk::Pixbuf> MainWindow::get_status_icon(const std::string& status) {
    std::string icon_name;
    if (status == "available") {
        icon_name = "emblem-ok";        // ✅ Vert
    } else if (status == "incorrect") {
        icon_name = "dialog-warning";   // ⚠️ Jaune/Orange
    } else {
        icon_name = "image-missing";    // ❌ Gris
    }
    return Gtk::IconTheme::get_default()->load_icon(icon_name, 16);
}

void MainWindow::on_game_selected() {
    auto selection = m_treeview_games.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;

    Gtk::TreeModel::Row row = *iter;

    Glib::ustring name_glib = row[m_columns.m_col_name];
    Glib::ustring title_glib = row[m_columns.m_col_title];

    std::string name = name_glib.raw();   // ou .latin1(), mais .raw() est mieux pour UTF-8
    std::string title = title_glib.raw();

    m_label_title.set_markup("<b>" + title + "</b>");
    m_label_info.set_text("ROM: " + name);
    m_button_play.set_sensitive(true);
}

void MainWindow::on_play_clicked() {
    auto row = dynamic_cast<GameRow*>(m_listbox_games.get_selected_row());
    if (!row) return;

    std::string rom_name = row->get_rom_name();
    std::cout << "Lancement du jeu : " << rom_name << std::endl;

    std::string command = "fbneo " + rom_name + " &";
    std::system(command.c_str());
}

void MainWindow::on_refresh_clicked() {
    std::cout << "Refresh clicked – will scan ROMs later" << std::endl;
}

void MainWindow::on_hide() {
    m_settings_panel.save_to_file("config.json");
    Gtk::Window::on_hide();  // Appelle la méthode parente
}

void MainWindow::populate_from_dat() {
    std::string dat_path = m_settings_panel.get_dat_path();
    if (dat_path.empty()) {
        std::cerr << "Erreur : chemin du DAT non défini" << std::endl;
        return;
    }

    auto games = DatParser::parse(dat_path);
    if (games.empty()) {
        std::cerr << "Aucun jeu chargé depuis : " << dat_path << std::endl;
        return;
    }

    std::string roms_path = m_settings_panel.get_roms_path();

    for (const auto& game : games) {
        Game mutable_game = game;
        RomScanner::check_availability(mutable_game, roms_path);

        auto row = *m_model_games->append();
        row[m_columns.m_col_icon] = get_status_icon(mutable_game.status);
        row[m_columns.m_col_name] = mutable_game.name;
        row[m_columns.m_col_title] = mutable_game.description;
        row[m_columns.m_col_year] = mutable_game.year;
        row[m_columns.m_col_manufacturer] = mutable_game.manufacturer;
    }
}