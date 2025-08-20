// src/ScanProgressDialog.cpp
#include "ScanProgressDialog.h"
#include "RomScanner.h"
#include <iostream>
#include <unistd.h>

ScanProgressDialog::ScanProgressDialog(Gtk::Window& parent)
    : Gtk::Dialog("🔍 ROM Scan Progress", parent, Gtk::DIALOG_DESTROY_WITH_PARENT) {
    
    set_modal(false);
    set_default_size(500, 200);
    set_resizable(false);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    
    // Setup content with better styling
    m_content_box.set_spacing(15);
    m_content_box.set_margin_start(25);
    m_content_box.set_margin_end(25);
    m_content_box.set_margin_top(20);
    m_content_box.set_margin_bottom(20);
    
    // Status label with better styling
    m_status_label.set_text("🚀 Preparing scan...");
    m_status_label.set_halign(Gtk::ALIGN_START);
    m_status_label.set_markup("<span size='large' weight='bold'>🚀 Preparing scan...</span>");
    
    // Progress bar with better appearance
    m_progress_bar.set_show_text(true);
    m_progress_bar.set_text("0%");
    m_progress_bar.set_size_request(-1, 25);
    
    // Details label with better styling
    m_details_label.set_text("");
    m_details_label.set_halign(Gtk::ALIGN_START);
    m_details_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_details_label.set_markup("<span style='italic' alpha='75%'></span>");
    
    // Cancel button with icon and better styling
    auto cancel_icon = Gdk::Pixbuf::create_from_file("assets/icons/cancel.svg", 16, 16);
    auto cancel_image = Gtk::make_managed<Gtk::Image>(cancel_icon);
    m_cancel_button.set_image(*cancel_image);
    m_cancel_button.set_label("Cancel");
    m_cancel_button.set_always_show_image(true);
    m_cancel_button.set_halign(Gtk::ALIGN_END);
    m_cancel_button.signal_clicked().connect(
        sigc::mem_fun(*this, &ScanProgressDialog::on_cancel_clicked));
    
    // Pack everything with better spacing
    m_content_box.pack_start(m_status_label, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_details_label, Gtk::PACK_SHRINK);
    
    // Button container for better alignment
    auto button_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL);
    button_box->set_halign(Gtk::ALIGN_END);
    button_box->pack_start(m_cancel_button, Gtk::PACK_SHRINK);
    m_content_box.pack_end(*button_box, Gtk::PACK_SHRINK);
    
    get_content_area()->add(m_content_box);
    show_all_children();
}

ScanProgressDialog::~ScanProgressDialog() {}

void ScanProgressDialog::start_scan(const std::vector<Game>& games, const std::string& roms_path) {
    m_cancel_requested = false;
    m_scanned_games.clear();
    
    int total = games.size();
    int found_count = 0;
    m_status_label.set_markup("<span size='large' weight='bold'>🔍 Scanning ROM files...</span>");
    
    for (size_t i = 0; i < games.size(); ++i) {
        if (m_cancel_requested) {
            m_status_label.set_markup("<span size='large' weight='bold' color='#ff6b6b'>❌ Scan cancelled</span>");
            m_cancel_button.set_label("Close");
            m_cancel_button.set_sensitive(true);
            break;
        }
        
        Game mutable_game = games[i];
        RomScanner::check_availability(mutable_game, roms_path);
        m_scanned_games.push_back(mutable_game);
        
        if (mutable_game.status == "available" || mutable_game.status == "incorrect") {
            found_count++;
        }
        
        update_progress(i + 1, total, mutable_game.name + " (Found: " + std::to_string(found_count) + ")");
        
        // Process UI events to handle cancel button
        while (Gtk::Main::events_pending()) {
            Gtk::Main::iteration();
        }
        
        // Small delay to see progress for debugging
        if (i % 100 == 0) {
            usleep(1000); // 1ms delay every 100 games
        }
    }
    
    if (!m_cancel_requested) {
        std::cout << "[DEBUG] ScanProgressDialog: Scan completed! Found " << found_count << " ROMs out of " << total << " games" << std::endl;
        m_status_label.set_markup("<span size='large' weight='bold' color='#51cf66'>✅ Scan completed!</span>");
        m_progress_bar.set_text("100%");
        m_details_label.set_markup("<span style='italic' alpha='75%'>" + std::to_string(total) + " games processed, " + std::to_string(found_count) + " ROMs found</span>");
        m_cancel_button.set_label("Close");
    }
}

void ScanProgressDialog::update_progress(int current, int total, const std::string& game_name) {
    double fraction = static_cast<double>(current) / total;
    m_progress_bar.set_fraction(fraction);
    
    int percentage = static_cast<int>(fraction * 100);
    m_progress_bar.set_text(std::to_string(percentage) + "%");
    
    m_details_label.set_markup("<span style='italic' alpha='75%'>Processing: " + game_name + "</span>");
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

void ScanProgressDialog::start_scan(const std::vector<Game>& games, const std::vector<std::string>& roms_paths) {
    m_cancel_requested = false;
    m_scanned_games.clear();
    
    int total = games.size();
    int found_count = 0;
    
    std::cout << "[DEBUG] ScanProgressDialog: Starting fresh scan of " << total << " games in " << roms_paths.size() << " directories" << std::endl;
    
    // Copy games (they should already have "missing" status from fresh DAT load)
    m_scanned_games = games;
    
    m_status_label.set_markup("<span size='large' weight='bold'>🔍 Scanning ROM files in multiple directories...</span>");
    
    for (size_t i = 0; i < m_scanned_games.size(); ++i) {
        if (m_cancel_requested) {
            m_status_label.set_markup("<span size='large' weight='bold' color='#ff6b6b'>❌ Scan cancelled</span>");
            m_cancel_button.set_label("Close");
            m_cancel_button.set_sensitive(true);
            break;
        }
        
        RomScanner::check_availability(m_scanned_games[i], roms_paths);
        
        if (m_scanned_games[i].status == "available" || m_scanned_games[i].status == "incorrect") {
            found_count++;
            if (found_count <= 5) {
                std::cout << "[DEBUG] Found ROM: " << m_scanned_games[i].name << " (" << m_scanned_games[i].status << ")" << std::endl;
            }
        }
        
        // Debug first few games
        if (i < 5) {
            std::cout << "[DEBUG] Game " << i << ": " << m_scanned_games[i].name << " → " << m_scanned_games[i].status << std::endl;
        }
        
        update_progress(i + 1, total, m_scanned_games[i].name + " (Found: " + std::to_string(found_count) + ")");
        
        // Process UI events to handle cancel button
        while (Gtk::Main::events_pending()) {
            Gtk::Main::iteration();
        }
        
        // Small delay to see progress for debugging
        if (i % 100 == 0) {
            usleep(1000); // 1ms delay every 100 games
        }
    }
    
    if (!m_cancel_requested) {
        std::cout << "[DEBUG] ScanProgressDialog: Scan completed! Found " << found_count << " ROMs out of " << total << " games" << std::endl;
        m_status_label.set_markup("<span size='large' weight='bold' color='#51cf66'>✅ Scan completed!</span>");
        m_progress_bar.set_text("100%");
        m_details_label.set_markup("<span style='italic' alpha='75%'>" + std::to_string(total) + " games processed, " + std::to_string(found_count) + " ROMs found</span>");
        m_cancel_button.set_label("Close");
    }
}