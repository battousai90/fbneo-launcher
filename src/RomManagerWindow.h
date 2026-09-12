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
#include "RomImportTab.h"
#include "RomLibraryTab.h"
#include "RomOutboxTab.h"

class RomManagerWindow : public Gtk::Window {
public:
    RomManagerWindow(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db);
    virtual ~RomManagerWindow();

    // Re-read config.json. Called by the owner before presenting the window so a
    // path changed in the Settings panel meanwhile is picked up.
    void reload_settings();

    // Called by the owner once a ROM scan it started finishes : covers the
    // "Move to library" loop (move → scan → the audit should show the set as
    // fixed now, not still flagged) as well as any other scan. Outbox/Quarantine
    // are cheap to refresh unconditionally; the audit only re-runs if one was
    // already performed this session, so this never starts an unrequested scan.
    void refresh_after_scan();

    // Bring one tab to the front: "library", "import", "outbox", "quarantine", "dat".
    void show_tab(const std::string& id);

    // Emitted when the user changes the DAT directory here, so the Settings panel
    // (which owns the same config.json key) can stay in sync.
    sigc::signal<void, std::string>& signal_dat_path_changed() { return m_sig_dat_path_changed; }
    // Emitted when the user asks for a DAT database update from this window.
    sigc::signal<void>&              signal_update_dat()       { return m_sig_update_dat; }
    // Emitted after "Move to library" moves files in place : no new path to add,
    // just a rescan of the existing ROM directories.
    sigc::signal<void>&              signal_scan_requested()   { return m_sig_scan_requested; }
    // Distinct from the above: that one fires as the tail of "Move to library"
    // and must not interrupt the flow, this one is a button the user pressed
    // and goes through the same confirmation the header button always had.
    sigc::signal<void>&              signal_rescan_requested() { return m_sig_rescan_requested; }

private:
    // ── Shell : title bar, tabs ──────────────────────────────────────────────
    void add_tab(const std::string& id, const std::string& icon_file,
                 const std::string& label, const std::string& subtitle);

    // ── Tab construction ─────────────────────────────────────────────────────
    void build_quarantine_tab();
    void build_dat_tab();

    // Library found repairable sets : copy their archives into the inbox and
    // let Import analyse them.
    void on_send_to_import(std::vector<std::string> archives);

    // ── Settings persistence (the "rom_manager" object in config.json) ────────
    void save_settings();
    // FBNeo executable path, owned by the Settings panel and read from config.json.
    std::string fbneo_executable() const;

    // Reads "roms_paths" from config.json. Called on the GTK main thread : the
    // roots belong to the Settings panel, not to this window.
    std::vector<std::string> read_roms_paths() const;

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
    Gtk::HeaderBar m_headerbar;
    Gtk::Label     m_header_sub;
    Gtk::Box       m_tabbar{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Stack     m_pages;
    struct Tab { std::string id; Gtk::ToggleButton* button; std::string subtitle; };
    std::vector<Tab> m_tabs;
    bool m_tab_switching = false;

    RomImportTab*  m_import  = nullptr;
    RomLibraryTab* m_library = nullptr;
    RomOutboxTab*  m_outbox  = nullptr;
    // A string of the "rom_manager" object in config.json (the outbox folder
    // now lives in Settings).
    std::string config_string(const char* key) const;

    // A tab is working : nothing that moves files may start, and the window
    // stays open.
    bool busy() const;
    // Lines from the tabs that have no log of their own yet go to Import's.
    void push_log(const std::string& msg);

    // Quarantine tab : sets "Quarantine incorrect" (Library tab) moved out of the
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

    sigc::signal<void, std::string> m_sig_dat_path_changed;
    sigc::signal<void>              m_sig_update_dat;
    sigc::signal<void>              m_sig_scan_requested;
    sigc::signal<void>              m_sig_rescan_requested;
};
