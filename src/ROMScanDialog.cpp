// src/ROMScanDialog.cpp
#include "ROMScanDialog.h"
#include "RomScanner.h"
#include <iostream>
#include <filesystem>
#include <set>
#include <zlib.h>
#include <zip.h>
#include <fstream>
#include <sstream>

extern uLong hex_to_crc(const std::string& hex);
extern uLong compute_crc32_in_zip(const std::string& zip_path, const std::string& rom_name);

ROMScanDialog::ROMScanDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths)
    : Gtk::Dialog("🔍 ROM Scan Progress", parent, Gtk::DIALOG_DESTROY_WITH_PARENT)
    , m_db(db)
    , m_roms_paths(roms_paths)
    , m_cancelled(false)
    , m_found_count(0)
    , m_scan_finished(false)
{
    set_default_size(600, 400);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    set_modal(true);
    
    // Title
    m_title_label.set_markup("<span size='x-large' weight='bold'>🔍 ROM Scanner</span>");
    m_title_label.set_halign(Gtk::ALIGN_CENTER);
    m_title_label.set_margin_bottom(10);
    
    // Progress section
    m_current_file_label.set_text("Initialization...");
    m_current_file_label.set_halign(Gtk::ALIGN_START);
    m_current_file_label.set_ellipsize(Pango::ELLIPSIZE_END);
    
    m_progress_bar.set_show_text(true);
    m_progress_bar.set_text("0%");
    m_progress_bar.set_fraction(0.0);
    
    m_percentage_label.set_text("0%");
    m_percentage_label.set_halign(Gtk::ALIGN_END);
    
    m_progress_box.pack_start(m_current_file_label, Gtk::PACK_SHRINK);
    m_progress_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    m_progress_box.pack_start(m_percentage_label, Gtk::PACK_SHRINK);
    
    // Log section
    m_log_title.set_halign(Gtk::ALIGN_START);
    m_log_title.set_margin_top(10);
    
    m_log_buffer = Gtk::TextBuffer::create();
    m_log_view.set_buffer(m_log_buffer);
    m_log_view.set_editable(false);
    m_log_view.set_cursor_visible(false);
    
    m_log_scrolled.add(m_log_view);
    m_log_scrolled.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_log_scrolled.set_size_request(-1, 200);
    
    // Buttons
    m_cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &ROMScanDialog::on_cancel_clicked));
    m_close_button.signal_clicked().connect([this]() { response(Gtk::RESPONSE_CLOSE); });
    m_close_button.set_sensitive(false);
    
    m_button_box.set_layout(Gtk::BUTTONBOX_END);
    m_button_box.pack_start(m_cancel_button);
    m_button_box.pack_start(m_close_button);
    
    // Pack everything
    m_main_box.set_margin_start(20);
    m_main_box.set_margin_end(20);
    m_main_box.set_margin_top(20);
    m_main_box.set_margin_bottom(20);
    
    m_main_box.pack_start(m_title_label, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_progress_box, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_log_title, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_log_scrolled, Gtk::PACK_EXPAND_WIDGET);
    m_main_box.pack_start(m_button_box, Gtk::PACK_SHRINK);
    
    get_content_area()->add(m_main_box);
    
    // Connect dispatchers
    m_progress_dispatcher.connect(sigc::mem_fun(*this, &ROMScanDialog::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &ROMScanDialog::on_scan_finished));
    
    show_all_children();
}

ROMScanDialog::~ROMScanDialog() {
    if (m_worker_thread.joinable()) {
        m_cancelled = true;
        m_worker_thread.join();
    }
}

void ROMScanDialog::start_scan() {
    m_cancelled = false;
    m_found_count = 0;
    m_scan_finished = false;
    
    add_log_message("🚀 Starting ROM scan...");
    
    // Start worker thread
    m_worker_thread = std::thread(&ROMScanDialog::worker_thread, this);
}

void ROMScanDialog::worker_thread() {
    try {
        // Get all games from database first
        std::vector<Game> all_games = m_db->getAllGames();
        add_log_message("📋 Database loaded: " + std::to_string(all_games.size()) + " games");
        
        // Reset all games to missing status with single SQL query
        add_log_message("🔄 Resetting all games to missing status...");
        m_db->resetAllGamesToMissing();
        add_log_message("✅ All games reset - now scanning for available ROMs");
        
        // Collect all ROM files from all directories
        std::vector<std::string> all_rom_files;
        for (const auto& roms_path : m_roms_paths) {
            if (!std::filesystem::exists(roms_path)) {
                add_log_message("⚠️  Directory not found: " + roms_path);
                continue;
            }
            
            add_log_message("📁 Scanning directory: " + roms_path);
            int files_in_dir = 0;
            
            for (const auto& entry : std::filesystem::directory_iterator(roms_path)) {
                if (entry.is_regular_file()) {
                    std::string filepath = entry.path().string();
                    std::string extension = entry.path().extension().string();
                    
                    // Check for supported ROM file types (.zip primarily for arcade)
                    if (extension == ".zip") {
                        all_rom_files.push_back(filepath);
                        files_in_dir++;
                    }
                }
            }
            
            add_log_message("  └─ " + std::to_string(files_in_dir) + " .zip files found");
        }
        
        add_log_message("📦 Total: " + std::to_string(all_rom_files.size()) + " ROM files to process");
        
        // NOUVELLE LOGIQUE PROPRE : ZIP -> fichiers -> nom+CRC -> query DB
        add_log_message("🔍 Processing ROM files with clean logic...");
        
        int games_processed = 0;
        for (const auto& rom_file : all_rom_files) {
            if (m_cancelled) break;
            
            std::string game_name = std::filesystem::path(rom_file).stem().string();
            
            // Update progress
            double progress = static_cast<double>(games_processed) / all_rom_files.size() * 100.0;
            m_current_progress = progress;
            m_current_file = game_name;
            m_current_message = "Processing: " + game_name;
            m_progress_dispatcher();
            
            // Use the new clean scan method
            if (games_processed < 5) {
                add_log_message("🔍 Processing " + game_name + ".zip");
            }
            
            RomScanner::scan_zip_file(rom_file, m_db);
            
            games_processed++;
            
            if (games_processed % 20 == 0 || games_processed == all_rom_files.size()) {
                add_log_message("📊 Processed " + std::to_string(games_processed) + "/" + std::to_string(all_rom_files.size()) + " ZIP files");
            }
        }
        
        // Count found games
        std::vector<Game> updated_games = m_db->getAllGames();
        m_found_count = 0;
        for (const auto& game : updated_games) {
            if (game.status == "available" || game.status == "incorrect") {
                m_found_count++;
            }
        }
        
        if (!m_cancelled) {
            add_log_message("🎉 Scan completed! " + std::to_string(m_found_count) + " games found from " + std::to_string(games_processed) + " files processed");
        }
        
        m_scan_finished = true;
        m_finished_dispatcher();
        
    } catch (const std::exception& e) {
        add_log_message("❌ Error: " + std::string(e.what()));
        m_scan_finished = true;
        m_finished_dispatcher();
    }
}

void ROMScanDialog::on_progress_update() {
    // Update progress UI
    m_current_file_label.set_text(m_current_message);
    m_progress_bar.set_fraction(m_current_progress / 100.0);
    m_percentage_label.set_text(std::to_string(static_cast<int>(m_current_progress)) + "%");
    m_progress_bar.set_text(std::to_string(static_cast<int>(m_current_progress)) + "%");
    
    // Add any pending log messages (thread-safe)
    for (const auto& msg : m_log_messages) {
        auto iter = m_log_buffer->end();
        m_log_buffer->insert(iter, msg + "\n");
    }
    
    if (!m_log_messages.empty()) {
        // Auto-scroll to bottom
        auto mark = m_log_buffer->get_insert();
        m_log_view.scroll_to(mark);
        m_log_messages.clear();
    }
}

void ROMScanDialog::on_scan_finished() {
    if (m_cancelled) {
        m_current_file_label.set_text("Scan cancelled");
        m_progress_bar.set_text("Cancelled");
    } else {
        m_current_file_label.set_text("Scan completed - " + std::to_string(m_found_count) + " games found");
        m_progress_bar.set_fraction(1.0);
        m_progress_bar.set_text("100%");
        m_percentage_label.set_text("100%");
    }
    
    m_cancel_button.set_sensitive(false);
    m_close_button.set_sensitive(true);
    
    // Process any remaining log messages
    on_progress_update();
}

void ROMScanDialog::add_log_message(const std::string& message) {
    std::cout << "[ROM SCAN] " << message << std::endl;
    
    // Thread-safe: add to pending messages and dispatch to main thread
    m_log_messages.push_back(message);
    m_progress_dispatcher();
}

void ROMScanDialog::on_cancel_clicked() {
    m_cancelled = true;
    m_cancel_button.set_sensitive(false);
    add_log_message("🛑 Cancelling...");
}