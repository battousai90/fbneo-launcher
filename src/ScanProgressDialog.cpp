// src/ScanProgressDialog.cpp
#include "ScanProgressDialog.h"
#include "RomScanner.h"
#include <iostream>

ScanProgressDialog::ScanProgressDialog(Gtk::Window& parent)
    : Gtk::Dialog("ROM Scan Progress", parent, Gtk::DIALOG_DESTROY_WITH_PARENT) {
    
    set_modal(false);  // Non-modal pour pouvoir la déplacer
    set_default_size(400, 150);
    set_resizable(false);
    
    // Setup content
    m_content_box.set_spacing(10);
    m_content_box.set_margin_left(20);
    m_content_box.set_margin_right(20);
    m_content_box.set_margin_top(10);
    m_content_box.set_margin_bottom(10);
    
    m_status_label.set_text("Preparing scan...");
    m_status_label.set_halign(Gtk::ALIGN_START);
    
    m_progress_bar.set_show_text(true);
    m_progress_bar.set_text("0%");
    
    m_details_label.set_text("");
    m_details_label.set_halign(Gtk::ALIGN_START);
    m_details_label.set_ellipsize(Pango::ELLIPSIZE_END);
    
    m_cancel_button.signal_clicked().connect(
        sigc::mem_fun(*this, &ScanProgressDialog::on_cancel_clicked));
    
    // Pack everything
    m_content_box.pack_start(m_status_label, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_details_label, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_cancel_button, Gtk::PACK_SHRINK);
    
    get_content_area()->add(m_content_box);
    show_all_children();
}

ScanProgressDialog::~ScanProgressDialog() {}

void ScanProgressDialog::start_scan(const std::vector<Game>& games, const std::string& roms_path) {
    m_cancel_requested = false;
    m_scanned_games.clear();
    
    int total = games.size();
    m_status_label.set_text("Scanning ROM files...");
    
    for (size_t i = 0; i < games.size(); ++i) {
        if (m_cancel_requested) {
            m_status_label.set_text("Scan cancelled");
            m_cancel_button.set_label("Close");
            m_cancel_button.set_sensitive(true);
            break;
        }
        
        Game mutable_game = games[i];
        RomScanner::check_availability(mutable_game, roms_path);
        m_scanned_games.push_back(mutable_game);
        
        update_progress(i + 1, total, mutable_game.name);
        
        // Process UI events to handle cancel button
        while (Gtk::Main::events_pending()) {
            Gtk::Main::iteration();
        }
    }
    
    if (!m_cancel_requested) {
        m_status_label.set_text("Scan completed!");
        m_progress_bar.set_text("100%");
        m_details_label.set_text(std::to_string(total) + " games processed");
        m_cancel_button.set_label("Close");
    }
}

void ScanProgressDialog::update_progress(int current, int total, const std::string& game_name) {
    double fraction = static_cast<double>(current) / total;
    m_progress_bar.set_fraction(fraction);
    
    int percentage = static_cast<int>(fraction * 100);
    m_progress_bar.set_text(std::to_string(percentage) + "%");
    
    m_details_label.set_text("Processing: " + game_name);
}

void ScanProgressDialog::on_cancel_clicked() {
    if (m_cancel_button.get_label() == "Close") {
        hide();
    } else {
        m_cancel_requested = true;
        m_cancel_button.set_sensitive(false);
        m_status_label.set_text("Cancelling scan...");
    }
}