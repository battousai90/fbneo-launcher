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
        
        int processed = 0;
        
        // Use the restored logic: check games that have ZIP files in ROM directories
        add_log_message("🔍 Checking games for ROM availability...");
        
        // Create a map of ROM files with their directory info for system detection
        std::vector<std::pair<std::string, std::string>> rom_file_with_path;
        for (const auto& rom_file : all_rom_files) {
            std::string game_name = std::filesystem::path(rom_file).stem().string();
            rom_file_with_path.push_back({game_name, rom_file});
        }
        
        add_log_message("📋 Found " + std::to_string(rom_file_with_path.size()) + " ROM files to check");
        
        // Process each ROM file with system detection
        int games_processed = 0;
        for (const auto& [game_name, rom_file] : rom_file_with_path) {
            if (m_cancelled) break;
            
            // Update progress
            double progress = static_cast<double>(games_processed) / rom_file_with_path.size() * 100.0;
            m_current_progress = progress;
            m_current_file = game_name;
            m_current_message = "Checking: " + game_name;
            m_progress_dispatcher();
            
            // Get ROM directory for this specific file
            std::string roms_path = std::filesystem::path(rom_file).parent_path().string();
            
            // Find the correct game by CRC verification instead of guessing by directory name
            // Get ALL games with this name from database
            std::vector<Game> candidate_games = m_db->getAllGamesWithName(game_name);
            
            if (!candidate_games.empty()) {
                // Debug first few games in detail
                if (games_processed < 5) {
                    add_log_message("🔍 " + game_name + " - testing " + std::to_string(candidate_games.size()) + " candidates");
                    for (const auto& cand : candidate_games) {
                        add_log_message("  → [" + cand.system + "] " + cand.description + " (" + std::to_string(cand.roms.size()) + " ROMs)");
                    }
                }
                
                // Test each candidate game to see which one matches les ROM CRCs


                Game matched_game;
                    for (const auto& candidate : candidate_games) {
                        Game game = m_db->getGame(candidate.name, candidate.system);
                        if (game.roms.empty()) continue;
                        std::string zip_path = roms_path + "/" + game.name + ".zip";

                        // Ouvre le ZIP et parcourt tous les fichiers
                        int zip_error = 0;
                        zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
                        if (!zip) continue;
                        zip_int64_t num_entries = zip_get_num_entries(zip, 0);
                        bool found = false;
                        // 1ère passe : nom normalisé + CRC
                        auto normalize = [](const std::string& s) {
                            std::string out;
                            for (char c : s) {
                                if (c == ' ' || c == '-' || c == '_' ) continue;
                                out += std::tolower(c);
                            }
                            return out;
                        };
                        std::string expected_name = game.roms[0].name;
                        std::string expected_name_norm = normalize(expected_name);
                        uLong expected_crc = hex_to_crc(game.roms[0].crc);
                        for (zip_uint64_t i = 0; i < num_entries; ++i) {
                            zip_stat_t sb;
                            if (zip_stat_index(zip, i, 0, &sb) == 0) {
                                std::string entry_name = sb.name;
                                std::string entry_name_norm = normalize(entry_name);
                                uLong actual_crc = compute_crc32_in_zip(zip_path, entry_name);
                                if (entry_name_norm == expected_name_norm && actual_crc == expected_crc) {
                                    RomScanner::check_availability_db(candidate.name, candidate.system, m_db, roms_path);
                                    matched_game = m_db->getGame(candidate.name, candidate.system);
                                    add_log_message("  ✓ CRC Match found: [" + matched_game.system + "] " + matched_game.description);
                                    found = true;
                                    break;
                                }
                            }
                        }
                        // 2ème passe : CRC seul si aucun nom ne matche
                        if (!found) {
                            for (zip_uint64_t i = 0; i < num_entries; ++i) {
                                zip_stat_t sb;
                                if (zip_stat_index(zip, i, 0, &sb) == 0) {
                                    std::string entry_name = sb.name;
                                    uLong actual_crc = compute_crc32_in_zip(zip_path, entry_name);
                                    if (actual_crc == expected_crc) {
                                        RomScanner::check_availability_db(candidate.name, candidate.system, m_db, roms_path);
                                        matched_game = m_db->getGame(candidate.name, candidate.system);
                                        add_log_message("  ✓ CRC Match found (CRC only): [" + matched_game.system + "] " + matched_game.description);
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                        zip_close(zip);
                        if (found) break;
                }

                if (!matched_game.name.empty()) {
                    m_found_count++;
                    add_log_message("✅ " + game_name + " [" + matched_game.system + "] (" + matched_game.status + ")");
                } else if (games_processed < 5) {
                    add_log_message("❌ " + game_name + " - No CRC matches found in any system");
                }
            } else if (games_processed < 10) {
                add_log_message("❓ " + game_name + " not found in database");
            }
            
            games_processed++;
            
            if (games_processed % 20 == 0 || games_processed == rom_file_with_path.size()) {
                add_log_message("📊 Checked " + std::to_string(games_processed) + "/" + std::to_string(rom_file_with_path.size()) + 
                               " files, found " + std::to_string(m_found_count) + " available games");
            }
        }
        
        processed = games_processed;
        
        if (!m_cancelled) {
            add_log_message("🎉 Scan completed! " + std::to_string(m_found_count) + " games found from " + std::to_string(processed) + " files processed");
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