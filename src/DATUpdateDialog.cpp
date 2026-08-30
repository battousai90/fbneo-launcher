// src/DATUpdateDialog.cpp
#include "DATUpdateDialog.h"
#include "i18n.h"
#include "DatParser.h"
#include "RomScanner.h"
#include <thread>
#include <iostream>
#include <filesystem>
#include <ctime>
#include <unordered_map>
#include <vector>

DATUpdateDialog::DATUpdateDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::string& dat_path)
    : Gtk::Dialog(_("DAT Update"), parent, true)
    , m_db(db)
    , m_dat_path(dat_path)
{
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    m_log_title.set_text(_("Details:"));
    m_cancel_button.set_label(_("Cancel"));
    m_close_button.set_label(_("Close"));

    set_default_size(600, 400);
    set_modal(true);
    
    // Title
    m_title_label.set_markup("<b>DAT Database Update</b>");
    m_title_label.set_margin_bottom(10);
    m_main_box.pack_start(m_title_label, Gtk::PACK_SHRINK);
    
    // Progress section
    m_current_file_label.set_text(_("Preparing..."));
    m_current_file_label.set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    m_progress_box.pack_start(m_current_file_label, Gtk::PACK_SHRINK);
    
    m_progress_bar.set_fraction(0.0);
    m_progress_bar.set_show_text(false);
    m_progress_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    
    m_percentage_label.set_text("0%");
    m_percentage_label.set_halign(Gtk::ALIGN_CENTER);
    m_progress_box.pack_start(m_percentage_label, Gtk::PACK_SHRINK);
    
    m_main_box.pack_start(m_progress_box, Gtk::PACK_SHRINK);
    
    // Log section
    m_log_title.set_text(_("Details:"));
    m_log_title.set_halign(Gtk::ALIGN_START);
    m_log_title.set_margin_top(10);
    m_main_box.pack_start(m_log_title, Gtk::PACK_SHRINK);
    
    m_log_buffer = Gtk::TextBuffer::create();
    m_log_view.set_buffer(m_log_buffer);
    m_log_view.set_editable(false);
    m_log_view.set_cursor_visible(false);
    
    m_log_scrolled.add(m_log_view);
    m_log_scrolled.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_log_scrolled.set_size_request(-1, 200);
    m_main_box.pack_start(m_log_scrolled, Gtk::PACK_EXPAND_WIDGET);
    
    // Buttons
    m_cancel_button.set_label(_("Cancel"));
    m_close_button.set_label(_("Close"));
    m_cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &DATUpdateDialog::on_cancel_clicked));
    m_close_button.signal_clicked().connect(sigc::mem_fun(*this, &DATUpdateDialog::hide));
    m_close_button.set_sensitive(false);
    
    m_button_box.set_layout(Gtk::BUTTONBOX_END);
    m_button_box.set_spacing(10);
    m_button_box.pack_start(m_cancel_button);
    m_button_box.pack_start(m_close_button);
    m_main_box.pack_start(m_button_box, Gtk::PACK_SHRINK);
    
    get_content_area()->pack_start(m_main_box);
    
    // Threading
    m_progress_dispatcher.connect(sigc::mem_fun(*this, &DATUpdateDialog::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &DATUpdateDialog::on_update_finished));
    
    show_all_children();
}

DATUpdateDialog::~DATUpdateDialog() {
    if (m_worker_thread.joinable()) {
        m_cancelled.store(true);
        m_worker_thread.join();
    }
}

void DATUpdateDialog::start_update() {
    m_cancelled.store(false);
    m_update_finished.store(false);

    add_log_message("🚀 Starting DAT update...");

    m_worker_thread = std::thread(&DATUpdateDialog::worker_thread, this);
}

void DATUpdateDialog::on_cancel_clicked() {
    m_cancelled.store(true);
    m_cancel_button.set_sensitive(false);
    add_log_message("❌ Cancellation requested...");
}

void DATUpdateDialog::worker_thread() {
    // Helper to append a log message + notify UI, all under the shared mutex.
    auto log = [this](const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(m_shared_mutex);
            m_log_messages.push_back(msg);
        }
        m_progress_dispatcher();
    };

    try {
        // Phase 1: Reset database
        update_progress(0.1, "", "Clearing database...");
        log("🗑️  Removing all existing data...");

        if (m_cancelled.load()) { m_update_finished.store(true); m_finished_dispatcher(); return; }

        // DIFF: capture current game statuses + ROM signatures BEFORE wiping the
        // games table, so unchanged games keep their availability status without a
        // full ROM re-scan (see Phase 4).
        log("🧬 Snapshotting current game statuses for diff...");
        std::unordered_map<std::string, std::string> old_snapshot = m_db->snapshotStatusSignatures();
        log("🧬 Captured " + std::to_string(old_snapshot.size()) + " game statuses");

        // Favourites and play history survive the wipe on their own: the games
        // table carries triggers that copy them out on delete and put them back
        // on insert (see DatabaseManager). Reported here so the operation is
        // visibly accounted for rather than silently trusted.
        log("⭐ " + std::to_string(m_db->protectedPlayerStats())
            + " game(s) with play history : carried across the rebuild");

        if (!m_db->clearAllData()) {
            log("❌ Error clearing database");
            m_update_finished.store(true);
            m_finished_dispatcher();
            return;
        }
        // NOTE: rom_cache and directory snapshots are intentionally PRESERVED.
        // The physical ROM files on disk are unaffected by a DAT refresh, so their
        // cached CRC/metadata stays valid. Only games whose ROM definition actually
        // changed have their cache entry invalidated afterwards (Phase 4), so the
        // next scan re-reads just the diff instead of the whole collection.

        log("✅ Database cleared (ROM cache preserved)");

        // Phase 2: Scan DAT files
        if (m_cancelled.load()) { m_update_finished.store(true); m_finished_dispatcher(); return; }

        update_progress(0.2, "", "Scanning DAT files...");
        log("📁 Searching for DAT files in: " + m_dat_path);

        std::vector<std::string> dat_files;
        if (std::filesystem::exists(m_dat_path)) {
            for (const auto& entry : std::filesystem::directory_iterator(m_dat_path)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                    dat_files.push_back(entry.path().string());
                }
            }
        }

        if (dat_files.empty()) {
            log("❌ No DAT files found in: " + m_dat_path);
            m_update_finished.store(true);
            m_finished_dispatcher();
            return;
        }

        log("📋 Found " + std::to_string(dat_files.size()) + " DAT files");

        // Phase 3: Loading DAT files
        double progress_per_file = 0.7 / dat_files.size(); // 70% for loading
        double current_progress = 0.2;

        for (size_t i = 0; i < dat_files.size() && !m_cancelled.load(); ++i) {
            const auto& filepath = dat_files[i];
            std::string filename = std::filesystem::path(filepath).filename().string();

            current_progress += progress_per_file;
            update_progress(current_progress, filename, "Loading...");
            log("📥 Loading: " + filename);

            // parseToDatabase now returns the number of games loaded (or -1 on error)
            int games_added = DatParser::parseToDatabase(filepath, m_db);

            if (games_added >= 0) {
                log("✅ " + filename + " loaded (" + std::to_string(games_added) + " games)");
            } else {
                log("❌ Error loading: " + filename);
            }
        }

        if (m_cancelled.load()) { m_update_finished.store(true); m_finished_dispatcher(); return; }

        // Phase 4: Finalization + DIFF apply
        update_progress(0.95, "", "Finalizing...");
        size_t final_game_count = m_db->getGameCount();
        log("📊 Total: " + std::to_string(final_game_count) + " games loaded");

        // Restore statuses for games whose ROM definition is unchanged, and invalidate
        // the ROM cache only for games that are new or whose definition changed.
        log("🔁 Applying diff (restoring statuses for unchanged games)...");
        std::vector<std::string> changed_zip_names;
        int restored = m_db->applyPreservedStatuses(old_snapshot, changed_zip_names);
        log("✅ Restored " + std::to_string(restored) + " game statuses : no re-scan needed");
        log("🔎 " + std::to_string(changed_zip_names.size()) + " new/changed games to re-evaluate");

        // Re-derive statuses for new/changed games directly from the content-addressed
        // cache (zip_contents) : zero disk I/O. Resolves everything, including clones
        // whose ROMs live in a parent ZIP, provided that ZIP was scanned at least once.
        update_progress(0.98, "", "Re-matching from cache...");
        log("⚡ Re-matching games from ROM content cache (no disk read)...");
        int rematched = RomScanner::rematch_from_cache(m_db);
        log("✅ Re-matched " + std::to_string(rematched) + " games from cache");

        // Fallback: for anything the cache could not resolve, invalidate its cache
        // entry so a subsequent ROM scan re-reads just those files.
        m_db->invalidateRomCacheForFiles(changed_zip_names);

        update_progress(1.0, "", "Complete!");
        log("🎉 Update completed! Statuses are up to date : a full re-scan is no longer required.");

    } catch (const std::exception& e) {
        log(std::string("💥 Error: ") + e.what());
    }

    m_update_finished.store(true);
    m_finished_dispatcher();
}

void DATUpdateDialog::update_progress(double percentage, const std::string& current_file, const std::string& message) {
    m_current_progress.store(percentage);
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_current_file = current_file;
        m_current_message = message;
    }
    m_progress_dispatcher();
}

void DATUpdateDialog::add_log_message(const std::string& message) {
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.push_back(message);
    }
    m_progress_dispatcher();
}

void DATUpdateDialog::on_progress_update() {
    // Snapshot shared state under lock, then update widgets without holding it.
    std::string current_file;
    std::string current_message;
    std::vector<std::string> pending_logs;
    double progress = m_current_progress.load();
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        current_file = m_current_file;
        current_message = m_current_message;
        pending_logs.swap(m_log_messages);
    }

    m_progress_bar.set_fraction(progress);
    m_percentage_label.set_text(std::to_string(static_cast<int>(progress * 100)) + "%");

    if (!current_file.empty()) {
        m_current_file_label.set_text(_("File: ") + current_file);
    } else if (!current_message.empty()) {
        m_current_file_label.set_text(current_message);
    }

    for (const auto& msg : pending_logs) {
        auto iter = m_log_buffer->end();
        m_log_buffer->insert(iter, msg + "\n");
    }

    if (!pending_logs.empty()) {
        auto mark = m_log_buffer->get_insert();
        m_log_view.scroll_to(mark);
    }
}

void DATUpdateDialog::on_update_finished() {
    m_cancel_button.set_sensitive(false);
    m_close_button.set_sensitive(true);

    if (m_cancelled.load()) {
        m_current_file_label.set_text(_("Operation cancelled"));
        add_log_message("⚠️  Operation cancelled by user");
    } else {
        m_current_file_label.set_text(_("Update completed!"));
    }

    on_progress_update();
}