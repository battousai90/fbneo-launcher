// src/MainWindow.cpp
#include "MainWindow.h"
#include <iostream>
#include "GameRow.h"

MainWindow::MainWindow() {
    set_title("fbneo-launcher");
    set_default_size(1000, 600);
    set_border_width(8);

    // --- Toolbar ---
    m_toolbar.set_spacing(10);
    m_toolbar.pack_start(m_button_refresh, Gtk::PACK_SHRINK);
    m_search_entry.set_placeholder_text("Search game...");
    m_toolbar.pack_start(m_search_entry);

    // --- Game List ---
    m_scrolled_games.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scrolled_games.add(m_listbox_games);
    m_listbox_games.set_selection_mode(Gtk::SELECTION_BROWSE); // Une sélection à la fois

    // select a row when clicked
    m_listbox_games.signal_row_activated().connect([this](Gtk::ListBoxRow*) {
        on_game_selected();
    });

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

    // Populate game list
    populate_game_list();

    show_all_children();
}

MainWindow::~MainWindow() {}

void MainWindow::populate_game_list() {
    std::vector<Game> games = {
        {"sf2", "Super Street Fighter II", "1993", "Capcom"},
        {"mslug", "Metal Slug", "1996", "SNK"},
        {"kof97", "King of Fighters '97", "1997", "SNK"},
        {"punisher", "The Punisher", "1993", "Capcom"},
        {"tmnt", "Teenage Mutant Ninja Turtles", "1989", "Konami"}
    };

    for (const auto& game : games) {
        auto row = Gtk::make_managed<GameRow>(game.title, game.name, game.manufacturer, game.year);
        m_listbox_games.append(*row);
    }
}

void MainWindow::on_game_selected() {
    auto row = dynamic_cast<GameRow*>(m_listbox_games.get_selected_row());
    if (!row) return;

    m_label_title.set_markup("<b>" + row->get_title() + "</b>");
    m_label_info.set_text("ROM: " + row->get_rom_name());
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
    // Ici, tu pourras relire le dossier des ROMs
}