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
#include <chrono>

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
        // Get game count from database (fast COUNT query)
        size_t total_games = m_db->getGameCount();
        add_log_message("📋 Database loaded: " + std::to_string(total_games) + " games");

        // Get current DAT timestamp
        time_t current_dat_timestamp = m_db->getLastDatTimestamp();

        // Cleanup cache: remove entries for files that no longer exist
        add_log_message("🧹 Cleaning up ROM cache...");
        m_db->cleanupRomCache(m_roms_paths);

        add_log_message("🔍 Checking for outdated ROM files...");

        // Get only outdated/new ROM files that need scanning
        std::vector<std::string> files_to_scan = m_db->getOutdatedRomFiles(m_roms_paths, current_dat_timestamp);

        if (files_to_scan.empty()) {
            add_log_message("✅ All ROM files are up to date - no scanning needed!");
            add_log_message("💡 Tip: ROMs will be re-scanned if you update DAT files or add/modify ZIP files");

            // Just count cached results
            size_t available_count = m_db->getGameCountByStatus("available");
            size_t incorrect_count = m_db->getGameCountByStatus("incorrect");
            m_found_count = available_count + incorrect_count;

            add_log_message("📊 Cache results: " + std::to_string(m_found_count) + " games found");
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        add_log_message("📦 Found " + std::to_string(files_to_scan.size()) + " ROM files to scan (new or modified)");

        // Only reset games to missing if we're doing an incremental scan
        // For full scans, we'll keep existing statuses and only update what changes
        if (files_to_scan.size() > 0) {
            add_log_message("🔄 Preparing for incremental scan...");
        }
        
        // NOUVELLE LOGIQUE PROPRE : ZIP -> fichiers -> nom+CRC -> query DB
        add_log_message("🔍 Processing ROM files with clean logic...");

        // Collect file metadata for cache (BEFORE transaction)
        struct FileMetadata {
            std::string filename;
            std::string filepath;
            time_t last_modified;
            size_t file_size;
        };
        std::vector<FileMetadata> scanned_files;
        scanned_files.reserve(files_to_scan.size());

        for (const auto& rom_file : files_to_scan) {
            std::string filename = std::filesystem::path(rom_file).filename().string();
            auto ftime = std::filesystem::last_write_time(std::filesystem::path(rom_file));
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            time_t last_modified = std::chrono::system_clock::to_time_t(sctp);
            size_t file_size = std::filesystem::file_size(rom_file);

            scanned_files.push_back({filename, rom_file, last_modified, file_size});
        }

        // BEGIN TRANSACTION for batch updates - MASSIVE performance boost
        if (!m_db->beginTransaction()) {
            add_log_message("❌ Failed to start transaction");
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        int games_processed = 0;
        for (const auto& rom_file : files_to_scan) {
            if (m_cancelled) break;

            std::string game_name = std::filesystem::path(rom_file).stem().string();

            // Update progress
            double progress = static_cast<double>(games_processed) / files_to_scan.size() * 100.0;
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

            if (games_processed % 20 == 0 || games_processed == files_to_scan.size()) {
                add_log_message("📊 Processed " + std::to_string(games_processed) + "/" + std::to_string(files_to_scan.size()) + " ZIP files");
            }
        }

        // COMMIT TRANSACTION - All game status updates done
        if (!m_cancelled) {
            if (!m_db->commitTransaction()) {
                add_log_message("❌ Failed to commit game updates");
                m_db->rollbackTransaction();
                m_scan_finished = true;
                m_finished_dispatcher();
                return;
            }
            add_log_message("✅ Game statuses updated");
        } else {
            m_db->rollbackTransaction();
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        // NOW register files in cache (AFTER transaction commit)
        add_log_message("💾 Updating ROM cache...");
        for (const auto& metadata : scanned_files) {
            m_db->registerRomFile(metadata.filename, metadata.filepath,
                                  metadata.last_modified, metadata.file_size, current_dat_timestamp);
        }
        add_log_message("✅ ROM cache updated with " + std::to_string(scanned_files.size()) + " files");
        
        // Count found games using fast SQL COUNT queries
        size_t available_count = m_db->getGameCountByStatus("available");
        size_t incorrect_count = m_db->getGameCountByStatus("incorrect");
        m_found_count = available_count + incorrect_count;
        
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