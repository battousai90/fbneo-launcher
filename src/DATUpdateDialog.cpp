// src/DATUpdateDialog.cpp
#include "DATUpdateDialog.h"
#include "DatParser.h"
#include <thread>
#include <iostream>
#include <filesystem>

DATUpdateDialog::DATUpdateDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::string& dat_path)
    : Gtk::Dialog("DAT Update", parent, true)
    , m_db(db)
    , m_dat_path(dat_path)
    , m_cancelled(false)
    , m_current_progress(0.0)
    , m_update_finished(false)
{
    set_default_size(600, 400);
    set_modal(true);
    
    // Title
    m_title_label.set_markup("<b>DAT Database Update</b>");
    m_title_label.set_margin_bottom(10);
    m_main_box.pack_start(m_title_label, Gtk::PACK_SHRINK);
    
    // Progress section
    m_current_file_label.set_text("Preparing...");
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
    m_log_title.set_text("Details:");
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
    m_cancel_button.set_label("Cancel");
    m_close_button.set_label("Close");
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
        m_cancelled = true;
        m_worker_thread.join();
    }
}

void DATUpdateDialog::start_update() {
    m_cancelled = false;
    m_update_finished = false;
    
    add_log_message("🚀 Starting DAT update...");
    
    m_worker_thread = std::thread(&DATUpdateDialog::worker_thread, this);
}

void DATUpdateDialog::on_cancel_clicked() {
    m_cancelled = true;
    m_cancel_button.set_sensitive(false);
    add_log_message("❌ Cancellation requested...");
}

void DATUpdateDialog::worker_thread() {
    try {
        // Phase 1: Reset database
        update_progress(0.1, "", "Clearing database...");
        m_log_messages.push_back("🗑️  Removing all existing data...");
        m_progress_dispatcher();
        
        if (m_cancelled) return;
        
        if (!m_db->clearAllData()) {
            m_log_messages.push_back("❌ Error clearing database");
            m_finished_dispatcher();
            return;
        }
        
        m_log_messages.push_back("✅ Database cleared");
        m_progress_dispatcher();
        
        // Phase 2: Scan DAT files
        if (m_cancelled) return;
        
        update_progress(0.2, "", "Scanning DAT files...");
        m_log_messages.push_back("📁 Searching for DAT files in: " + m_dat_path);
        m_progress_dispatcher();
        
        std::vector<std::string> dat_files;
        if (std::filesystem::exists(m_dat_path)) {
            for (const auto& entry : std::filesystem::directory_iterator(m_dat_path)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                    dat_files.push_back(entry.path().string());
                }
            }
        }
        
        if (dat_files.empty()) {
            m_log_messages.push_back("❌ No DAT files found in: " + m_dat_path);
            m_finished_dispatcher();
            return;
        }
        
        m_log_messages.push_back("📋 Found " + std::to_string(dat_files.size()) + " DAT files");
        m_progress_dispatcher();
        
        // Phase 3: Loading DAT files
        double progress_per_file = 0.7 / dat_files.size(); // 70% for loading
        double current_progress = 0.2;
        
        for (size_t i = 0; i < dat_files.size() && !m_cancelled; ++i) {
            const auto& filepath = dat_files[i];
            std::string filename = std::filesystem::path(filepath).filename().string();
            
            current_progress += progress_per_file;
            update_progress(current_progress, filename, "Loading...");
            m_log_messages.push_back("📥 Loading: " + filename);
            m_progress_dispatcher();
            
            // Get count before loading to calculate games added
            size_t games_before = m_db->getAllGames().size();
            
            if (DatParser::parseToDatabase(filepath, m_db)) {
                // Get count after loading to show games added
                size_t games_after = m_db->getAllGames().size();
                size_t games_added = games_after - games_before;
                m_log_messages.push_back("✅ " + filename + " loaded (" + std::to_string(games_added) + " games)");
            } else {
                m_log_messages.push_back("❌ Error loading: " + filename);
            }
            m_progress_dispatcher();
        }
        
        if (m_cancelled) return;
        
        // Phase 4: Finalization
        update_progress(0.95, "", "Finalizing...");
        auto final_games = m_db->getAllGames();
        m_log_messages.push_back("📊 Total: " + std::to_string(final_games.size()) + " games loaded");
        m_progress_dispatcher();
        
        update_progress(1.0, "", "Complete!");
        m_log_messages.push_back("🎉 Update completed successfully!");
        
    } catch (const std::exception& e) {
        m_log_messages.push_back("💥 Error: " + std::string(e.what()));
    }
    
    m_update_finished = true;
    m_finished_dispatcher();
}

void DATUpdateDialog::update_progress(double percentage, const std::string& current_file, const std::string& message) {
    m_current_progress = percentage;
    m_current_file = current_file;
    m_current_message = message;
}

void DATUpdateDialog::add_log_message(const std::string& message) {
    m_log_messages.push_back(message);
}

void DATUpdateDialog::on_progress_update() {
    m_progress_bar.set_fraction(m_current_progress);
    m_percentage_label.set_text(std::to_string(static_cast<int>(m_current_progress * 100)) + "%");
    
    if (!m_current_file.empty()) {
        m_current_file_label.set_text("File: " + m_current_file);
    } else if (!m_current_message.empty()) {
        m_current_file_label.set_text(m_current_message);
    }
    
    // Add new log messages
    for (const auto& msg : m_log_messages) {
        auto iter = m_log_buffer->end();
        m_log_buffer->insert(iter, msg + "\n");
    }
    m_log_messages.clear();
    
    // Auto-scroll to bottom
    auto mark = m_log_buffer->get_insert();
    m_log_view.scroll_to(mark);
}

void DATUpdateDialog::on_update_finished() {
    m_cancel_button.set_sensitive(false);
    m_close_button.set_sensitive(true);
    
    if (m_cancelled) {
        m_current_file_label.set_text("Operation cancelled");
        add_log_message("⚠️  Operation cancelled by user");
    } else {
        m_current_file_label.set_text("Update completed!");
    }
    
    on_progress_update();
}