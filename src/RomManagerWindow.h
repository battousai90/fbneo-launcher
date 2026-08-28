// src/RomManagerWindow.h
//
// Dedicated, non-modal window gathering everything that manipulates ROM files:
// the inbox → outbox import/rebuild flow, a view of the produced outbox, and DAT
// management (generation from FBNeo, database refresh).
//
// It never writes inside the configured roms_paths. Analyse/Fix produce a separate
// outbox tree laid out like the DATs; the user then decides whether to copy it over
// or simply add the outbox to their ROM directories and run a normal scan.
#pragma once

#include <gtkmm.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DatabaseManager.h"
#include "RomAudit.h"
#include "RomCleanup.h"
#include "RomInbox.h"

class RomManagerWindow : public Gtk::Window {
public:
    // What a row represents. Set rows carry a RomInbox::Action; the extra kinds
    // cover per-ROM detail rows and the archives that matched no DAT entry at all.
    enum ResultKind {
        KIND_MOVE = 0, KIND_REBUILD, KIND_INCOMPLETE, KIND_IN_LIBRARY,
        KIND_UNKNOWN, KIND_MISSING_ROM, KIND_SOURCED_ROM
    };

    RomManagerWindow(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db);
    virtual ~RomManagerWindow();

    // Re-read config.json. Called by the owner before presenting the window so a
    // path changed in the Settings panel meanwhile is picked up.
    void reload_settings();

    // Called by the owner once a ROM scan it started finishes — covers the
    // "Move to library" loop (move → scan → the audit should show the set as
    // fixed now, not still flagged) as well as any other scan. Outbox/Quarantine
    // are cheap to refresh unconditionally; the audit only re-runs if one was
    // already performed this session, so this never starts an unrequested scan.
    void refresh_after_scan();

    // Emitted when the user changes the DAT directory here, so the Settings panel
    // (which owns the same config.json key) can stay in sync.
    sigc::signal<void, std::string>& signal_dat_path_changed() { return m_sig_dat_path_changed; }
    // Emitted when the user asks for a DAT database update from this window.
    sigc::signal<void>&              signal_update_dat()       { return m_sig_update_dat; }
    // Emitted after "Move to library" moves files in place — no new path to add,
    // just a rescan of the existing ROM directories.
    sigc::signal<void>&              signal_scan_requested()   { return m_sig_scan_requested; }

private:
    // ── Tab construction ─────────────────────────────────────────────────────
    void build_import_tab();
    void build_library_tab();
    void build_outbox_tab();
    void build_quarantine_tab();
    void build_dat_tab();

    // ── Library audit ────────────────────────────────────────────────────────
    void worker_audit();
    void on_audit_clicked();
    void populate_audit();
    void on_audit_filter_changed();
    bool audit_row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void on_export_audit();
    bool on_audit_button_press(GdkEventButton* event);
    void flash_audit_status(const Glib::ustring& text);
    void copy_audit_value(const Glib::ustring& value);
    void on_quarantine_clicked();
    void on_audit_row_toggled(const Glib::ustring& path);

    // ── Settings persistence (the "rom_manager" object in config.json) ────────
    void save_settings();
    // FBNeo executable path, owned by the Settings panel and read from config.json.
    std::string fbneo_executable() const;

    // Reads "roms_paths" from config.json. Called on the GTK main thread — the
    // roots belong to the Settings panel, not to this window.
    std::vector<std::string> read_roms_paths() const;

    // ── Import flow ──────────────────────────────────────────────────────────
    void on_analyze_clicked();
    void on_fix_clicked();
    void on_cancel_clicked();
    void on_row_toggled(const Glib::ustring& path);
    void populate_results();
    void update_summary();
    void set_busy(bool busy);

    // Worker plumbing — same pattern as ROMScanDialog: a std::thread publishing
    // under a mutex, and Glib::Dispatcher to hop back onto the GTK main thread.
    void worker_analyze();
    void worker_apply();
    void on_progress_update();
    void on_worker_finished();
    void push_progress(double pct, const std::string& msg);
    void push_log(const std::string& msg);
    RomInbox::Callbacks make_callbacks();

    // ── Outbox tab ───────────────────────────────────────────────────────────
    void refresh_outbox_view();
    void on_open_outbox_clicked();
    void on_move_to_library_clicked();
    void on_outbox_row_toggled(const Glib::ustring& path);

    // ── Quarantine tab ───────────────────────────────────────────────────────
    void refresh_quarantine_view();
    void on_open_quarantine_clicked();
    void on_purge_quarantine_clicked();

    // ── DAT tab ──────────────────────────────────────────────────────────────
    void refresh_dat_list();

    void on_browse(Gtk::Entry* entry);

    std::shared_ptr<DatabaseManager> m_db;
    Gtk::Window& m_parent;

    // ── Layout ───────────────────────────────────────────────────────────────
    Gtk::Notebook m_notebook;

    // Import tab
    Gtk::Box    m_import_box{Gtk::ORIENTATION_VERTICAL, 8};
    Gtk::Grid   m_paths_grid;
    Gtk::Label  m_label_inbox{"Inbox:"};
    Gtk::Entry  m_entry_inbox;
    Gtk::Button m_btn_browse_inbox{"Browse..."};
    Gtk::CheckButton m_check_recursive{"Scan the inbox recursively"};
    Gtk::InfoBar m_infobar;
    Gtk::Label   m_infobar_label;

    Gtk::ProgressBar m_progress;
    Gtk::Label       m_current_label;

    Gtk::ScrolledWindow m_results_scroll;
    Gtk::TreeView       m_results_view;
    Gtk::Label          m_summary_label;

    Gtk::ScrolledWindow m_log_scroll;
    Gtk::TextView       m_log_view;
    Glib::RefPtr<Gtk::TextBuffer> m_log_buffer;

    Gtk::ButtonBox m_import_buttons{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button    m_btn_analyze{"Analyse"};
    Gtk::Button    m_btn_fix{"Fix"};
    Gtk::Button    m_btn_cancel{"Cancel"};
    Gtk::Button    m_btn_close{"Close"};

    // Two-level model, RomVault style: one row per set, expandable into one row
    // per ROM (what is missing, what is borrowed from elsewhere).
    struct ResultColumns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          include;
        Gtk::TreeModelColumn<bool>          actionable;   // gates the toggle
        Gtk::TreeModelColumn<bool>          is_set;       // parent row?
        Gtk::TreeModelColumn<Glib::ustring> game;
        Gtk::TreeModelColumn<Glib::ustring> system;
        Gtk::TreeModelColumn<Glib::ustring> action;
        Gtk::TreeModelColumn<Glib::ustring> colour;       // status foreground
        Gtk::TreeModelColumn<Glib::ustring> destination;
        Gtk::TreeModelColumn<Glib::ustring> details;
        Gtk::TreeModelColumn<Glib::ustring> crc;          // detected CRC32, hex — empty when not a single file
        Gtk::TreeModelColumn<unsigned int>  index;        // into m_report.sets
        Gtk::TreeModelColumn<int>           kind;         // ResultKind
        ResultColumns() {
            add(include); add(actionable); add(is_set); add(game); add(system);
            add(action); add(colour); add(destination); add(details); add(crc); add(index); add(kind);
        }
    };
    ResultColumns m_cols;
    Glib::RefPtr<Gtk::TreeStore>     m_results_model;
    Glib::RefPtr<Gtk::TreeModelFilter> m_results_filter;

    // Status filter above the list ("show only fixable / missing / unknown").
    Gtk::Box         m_filter_box{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::ComboBoxText m_combo_filter;
    Gtk::Button       m_btn_export{"Export missing list..."};
    Gtk::Button       m_btn_select_all{"Select all"};
    Gtk::Button       m_btn_select_none{"Select none"};

    // Coloured counter pills, RomVault's statistics bar.
    Gtk::Box   m_stats_box{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Label m_stat_complete, m_stat_fixable, m_stat_missing,
               m_stat_library, m_stat_unknown, m_stat_selected;

    void build_stats_bar();
    void update_stats();
    bool row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void on_filter_changed();
    void on_export_missing();
    void set_all_selected(bool on);
    void append_rom_children(const Gtk::TreeModel::Row& parent, const RomInbox::SetPlan& s);

    // ── Library tab: what is actually wrong with the collection ──────────────
    Gtk::Box   m_audit_box{Gtk::ORIENTATION_VERTICAL, 8};
    Gtk::Label m_audit_intro;
    Gtk::Box   m_audit_stats{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Label m_astat_total, m_astat_available, m_astat_incorrect,
               m_astat_missing, m_astat_repairable;
    Gtk::Box          m_audit_filter_box{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::ComboBoxText m_audit_filter;
    Gtk::ProgressBar  m_audit_progress;
    Gtk::Label        m_audit_current;
    Gtk::ScrolledWindow m_audit_scroll;
    Gtk::TreeView       m_audit_view;
    Gtk::ButtonBox m_audit_buttons{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button    m_btn_audit{"Audit library"};
    Gtk::Button    m_btn_quarantine{"Quarantine incorrect"};
    Gtk::Button    m_btn_export_audit{"Export report..."};
    struct AuditColumns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> system;
        Gtk::TreeModelColumn<Glib::ustring> status;
        Gtk::TreeModelColumn<Glib::ustring> colour;
        Gtk::TreeModelColumn<Glib::ustring> detail;
        Gtk::TreeModelColumn<Glib::ustring> expected_zip; // set's DAT short name, e.g. "mslug"
        Gtk::TreeModelColumn<Glib::ustring> parent;       // cloneof short name, empty if original
        Gtk::TreeModelColumn<bool>          is_game;
        Gtk::TreeModelColumn<bool>          repairable;
        Gtk::TreeModelColumn<Glib::ustring> gstatus;   // parent status, for filtering
        Gtk::TreeModelColumn<bool>          include;       // checked for quarantine?
        Gtk::TreeModelColumn<bool>          quarantinable; // gates the toggle
        Gtk::TreeModelColumn<Glib::ustring> archive_path;  // quarantine source, game rows only
        Gtk::TreeModelColumn<Glib::ustring> dat_header;    // quarantine destination subfolder
        // Set only for available/repairable sets whose archive holds entries no
        // DAT rom needs — checking such a row extracts just those entries into
        // quarantine instead of moving the whole (otherwise fine) archive.
        Gtk::TreeModelColumn<bool>          has_extras;
        AuditColumns() {
            add(name); add(system); add(status); add(colour);
            add(detail); add(expected_zip); add(parent);
            add(is_game); add(repairable); add(gstatus);
            add(include); add(quarantinable); add(archive_path); add(dat_header);
            add(has_extras);
        }
    };
    AuditColumns m_acols;
    Glib::RefPtr<Gtk::TreeStore>       m_audit_model;
    Glib::RefPtr<Gtk::TreeModelFilter> m_audit_filter_model;
    RomAudit::Report m_audit;
    bool m_audit_ever_run = false; // gates the auto-refresh in refresh_after_scan()
    std::vector<std::string> m_job_roms_paths;   // snapshot for the worker

    // Outbox tab
    Gtk::Box    m_outbox_box{Gtk::ORIENTATION_VERTICAL, 8};
    Gtk::Grid   m_outbox_path_grid;
    Gtk::Label  m_label_outbox{"Outbox:"};
    Gtk::Entry  m_entry_outbox;
    Gtk::Button m_btn_browse_outbox{"Browse..."};
    Gtk::Label  m_outbox_summary;
    Gtk::ScrolledWindow m_outbox_scroll;
    Gtk::TreeView       m_outbox_view;
    Gtk::ButtonBox m_outbox_buttons{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button m_btn_open_outbox{"Open folder"};
    Gtk::Button m_btn_move_to_library{"Move to library"};
    Gtk::Button m_btn_refresh_outbox{"Refresh"};

    struct OutboxColumns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          include;   // checked for "Move to library"?
        Gtk::TreeModelColumn<bool>          is_zip;    // gates the toggle (zip rows only)
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> count;
        Gtk::TreeModelColumn<Glib::ustring> size;
        Gtk::TreeModelColumn<Glib::ustring> full_path; // zip rows only
        OutboxColumns() { add(include); add(is_zip); add(name); add(count); add(size); add(full_path); }
    };
    OutboxColumns m_outbox_cols;
    Glib::RefPtr<Gtk::TreeStore> m_outbox_model;

    // Quarantine tab — sets "Quarantine incorrect" (Library tab) moved out of the
    // ROM library because the audit could not repair them (wrong data, no good
    // copy anywhere else). Nothing here is auto-deleted; Purge is explicit.
    Gtk::Box    m_quarantine_box{Gtk::ORIENTATION_VERTICAL, 8};
    Gtk::Grid   m_quarantine_grid;
    Gtk::Label  m_label_quarantine{"Quarantine directory:"};
    Gtk::Entry  m_entry_quarantine;
    Gtk::Button m_btn_browse_quarantine{"Browse..."};
    Gtk::Label  m_quarantine_summary;
    Gtk::ScrolledWindow m_quarantine_scroll;
    Gtk::TreeView       m_quarantine_view;
    Gtk::ButtonBox m_quarantine_buttons{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button m_btn_open_quarantine{"Open folder"};
    Gtk::Button m_btn_refresh_quarantine{"Refresh"};
    Gtk::Button m_btn_purge_quarantine{"Purge quarantine"};

    struct QuarantineColumns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> count;
        Gtk::TreeModelColumn<Glib::ustring> size;
        QuarantineColumns() { add(name); add(count); add(size); }
    };
    QuarantineColumns m_quarantine_cols;
    Glib::RefPtr<Gtk::TreeStore> m_quarantine_model;

    // DAT tab
    Gtk::Box    m_dat_box{Gtk::ORIENTATION_VERTICAL, 8};
    Gtk::Grid   m_dat_grid;
    Gtk::Label  m_label_dat{"DAT directory:"};
    Gtk::Entry  m_entry_dat;
    Gtk::Button m_btn_browse_dat{"Browse..."};
    Gtk::Button m_btn_generate_dat{"Generate DAT files from FBNeo"};
    Gtk::Button m_btn_update_dat{"Update database from DAT files"};
    Gtk::ScrolledWindow m_dat_scroll;
    Gtk::TreeView       m_dat_view;

    struct DatColumns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> filename;
        Gtk::TreeModelColumn<Glib::ustring> games;
        Gtk::TreeModelColumn<Glib::ustring> modified;
        DatColumns() { add(filename); add(games); add(modified); }
    };
    DatColumns m_dat_cols;
    Glib::RefPtr<Gtk::ListStore> m_dat_model;

    // ── Worker state ─────────────────────────────────────────────────────────
    enum class Job { None, Analyze, Apply, Audit };
    Job         m_job = Job::None;
    std::thread m_worker;
    Glib::Dispatcher m_progress_dispatcher;
    Glib::Dispatcher m_finished_dispatcher;

    mutable std::mutex  m_shared_mutex;
    std::atomic<double> m_progress_value{0.0};
    std::atomic<bool>   m_cancelled{false};
    std::atomic<bool>   m_busy{false};
    std::string              m_current_message;
    std::vector<std::string> m_log_messages;

    RomInbox::Report      m_report;       // worker-written, read on the main thread
    RomInbox::ApplyResult m_apply_result; // after joining only

    // Snapshot of the entry/checkbox values, taken on the GTK main thread before
    // the worker starts. The worker must never touch a widget: GTK is not
    // thread-safe, and reading an Entry from another thread is undefined behaviour.
    std::string m_job_inbox;
    std::string m_job_outbox;
    bool        m_job_recursive = false;

    sigc::signal<void, std::string> m_sig_dat_path_changed;
    sigc::signal<void>              m_sig_update_dat;
    sigc::signal<void>              m_sig_scan_requested;
};
