// src/RomManagerWindow.cpp
#include "RomManagerWindow.h"
#include "RomArchive.h"
#include "RomImportTab.h"
#include "RomLibraryTab.h"
#include "RomOutboxTab.h"
#include "RomResolve.h"
#include "RomManifest.h"
#include "SettingsUi.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "GenerateDAT.h"
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

// Same throttle as ROMScanDialog: emitting one dispatcher signal per processed file
// floods the GTK main loop and makes the window look hung.
constexpr int UI_DISPATCH_INTERVAL_MS = 100;

std::string human_size(uintmax_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10.0 && u > 0 ? 1 : 0) << v << ' ' << units[u];
    return os.str();
}

std::string join_preview(const std::vector<std::string>& items, size_t max_items) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < max_items; ++i) {
        if (i) out += ", ";
        out += items[i];
    }
    if (items.size() > max_items)
        out += ", … (+" + std::to_string(items.size() - max_items) + ")";
    return out;
}

} // namespace

RomManagerWindow::RomManagerWindow(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db)
    : m_db(db), m_parent(parent)
{
    namespace ui = SettingsUi;
    set_title(_("ROM Management"));
    set_default_size(1240, 860);
    set_transient_for(parent);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    get_style_context()->add_class("cc-window");
    get_style_context()->add_class("set-window");

    // ── Title bar : the Settings window's, same tile, same close ─────────
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    head->pack_start(*ui::tile("database.svg", 21, 36, /*accent=*/true), Gtk::PACK_SHRINK);
    auto* head_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    head_txt->set_valign(Gtk::ALIGN_CENTER);
    auto* head_title = Gtk::make_managed<Gtk::Label>(_("ROM Management"));
    head_title->set_xalign(0.0f);
    head_title->get_style_context()->add_class("set-head-title");
    m_header_sub.set_xalign(0.0f);
    m_header_sub.get_style_context()->add_class("set-head-sub");
    head_txt->pack_start(*head_title,  Gtk::PACK_SHRINK);
    head_txt->pack_start(m_header_sub, Gtk::PACK_SHRINK);
    head->pack_start(*head_txt, Gtk::PACK_SHRINK);

    auto* close = Gtk::make_managed<Gtk::Button>();
    close->set_image(*ui::image("bc-close.svg", 18));
    close->set_tooltip_text(_("Close"));
    close->get_style_context()->add_class("set-close");
    close->set_valign(Gtk::ALIGN_CENTER);
    close->signal_clicked().connect([this] { if (!busy()) hide(); });

    m_headerbar.set_show_close_button(false);
    m_headerbar.pack_start(*head);
    m_headerbar.pack_end(*close);
    m_headerbar.set_custom_title(*Gtk::make_managed<Gtk::Box>());
    m_headerbar.show_all();
    set_titlebar(m_headerbar);

    // ── Tabs ──────────────────────────────────────────────────────────────
    m_import = Gtk::make_managed<RomImportTab>(m_db, [this] {
        RomImportTab::Paths p;
        p.outbox     = config_string("outbox_path");
        p.quarantine = m_entry_quarantine.get_text().raw();
        return p;
    });
    m_import->signal_outbox_changed().connect([this] { m_outbox->refresh(); refresh_quarantine_view(); });

    m_outbox = Gtk::make_managed<RomOutboxTab>(m_db, [this] {
        RomOutboxTab::Paths p;
        p.outbox     = config_string("outbox_path");
        p.quarantine = m_entry_quarantine.get_text().raw();
        p.roms_paths = read_roms_paths();
        return p;
    });
    m_outbox->signal_scan_requested().connect([this] { m_sig_scan_requested.emit(); });
    m_outbox->signal_quarantine_changed().connect([this] { refresh_quarantine_view(); });

    m_library = Gtk::make_managed<RomLibraryTab>(m_db, [this] {
        RomLibraryTab::Paths p;
        p.roms_paths = read_roms_paths();
        p.inbox      = m_import->inbox_path();
        p.quarantine = m_entry_quarantine.get_text().raw();
        return p;
    });
    m_library->signal_rescan_requested().connect([this] { m_sig_rescan_requested.emit(); });
    m_library->signal_scan_requested().connect([this] { m_sig_scan_requested.emit(); });
    m_library->signal_log().connect([this](std::string line) { push_log(line); });
    m_library->signal_send_to_import().connect(sigc::mem_fun(*this, &RomManagerWindow::on_send_to_import));

    build_quarantine_tab();
    build_dat_tab();

    m_tabbar.get_style_context()->add_class("set-tabbar");
    m_pages.set_transition_type(Gtk::STACK_TRANSITION_TYPE_NONE);
    m_pages.add(*m_library,       "library");
    m_pages.add(*m_import,        "import");
    m_pages.add(*m_outbox,        "outbox");
    m_pages.add(m_quarantine_box, "quarantine");
    m_pages.add(m_dat_box,        "dat");
    add_tab("library",    "bc-folder.svg",   _("Library"),    _("Scan your ROM library and compare it with DAT files."));
    add_tab("import",     "bc-download.svg", _("Import"),     _("Analyse and repair ROMs before they reach your library."));
    add_tab("outbox",     "bc-package.svg",  _("Outbox"),     _("Repaired ROMs, ready to be moved to your library."));
    add_tab("quarantine", "bc-shield.svg",   _("Quarantine"), _("Files that could not be used, or were rejected."));
    add_tab("dat",        "bc-file.svg",     _("DAT"),        _("The DAT files your library is compared with."));

    auto* shell = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    shell->pack_start(m_tabbar, Gtk::PACK_SHRINK);
    shell->pack_start(m_pages,  Gtk::PACK_EXPAND_WIDGET);
    add(*shell);

    reload_settings();
    show_all_children();
    show_tab("library");
}

void RomManagerWindow::add_tab(const std::string& id, const std::string& icon_file,
                               const std::string& label, const std::string& subtitle) {
    namespace ui = SettingsUi;
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
    box->set_halign(Gtk::ALIGN_CENTER);
    box->pack_start(*ui::image(icon_file, 17), Gtk::PACK_SHRINK);
    box->pack_start(*Gtk::make_managed<Gtk::Label>(label), Gtk::PACK_SHRINK);
    btn->add(*box);
    btn->get_style_context()->add_class("set-tab");
    btn->set_active(m_tabs.empty());
    // "clicked", not "toggled" : the same reasoning as SettingsPanel::add_tab.
    btn->signal_clicked().connect([this, id] { show_tab(id); });
    m_tabs.push_back({id, btn, subtitle});
    m_tabbar.pack_start(*btn, Gtk::PACK_SHRINK);
}

void RomManagerWindow::show_tab(const std::string& id) {
    if (m_tab_switching) return;
    m_tab_switching = true;
    for (auto& t : m_tabs) {
        t.button->set_active(t.id == id);
        if (t.id == id) m_header_sub.set_text(t.subtitle);
    }
    m_pages.set_visible_child(id);
    m_tab_switching = false;
}

// Library hands over the archives of sets it found repairable : Import takes
// the stage and copies them in, then analyses straight away.
void RomManagerWindow::on_send_to_import(std::vector<std::string> archives) {
    if (busy()) return;
    show_tab("import");
    m_import->receive(archives);
}

bool RomManagerWindow::busy() const {
    return (m_import && m_import->busy()) || (m_library && m_library->busy()) || (m_outbox && m_outbox->busy());
}

std::string RomManagerWindow::config_string(const char* key) const {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) {} }
    if (j.contains("rom_manager") && j["rom_manager"].is_object()) {
        const auto& rm = j["rom_manager"];
        if (rm.contains(key) && rm[key].is_string()) return rm[key].get<std::string>();
    }
    return "";
}

// Every line an old-style tab (Outbox, Quarantine) or Library has to say goes
// to the Import log panel, the one log the window has until those tabs get
// their own : and to debug.log, which is what survives the window.
void RomManagerWindow::push_log(const std::string& msg) {
    std::cerr << "[ROM-MANAGER] " << msg << std::endl;
    if (m_import) m_import->log(msg);
}

RomManagerWindow::~RomManagerWindow() = default;

// ── Import tab ───────────────────────────────────────────────────────────────

void RomManagerWindow::on_browse(Gtk::Entry* entry) {
    Gtk::FileChooserDialog dialog(*this, _("Select Folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);
    auto current = entry->get_text();
    if (!current.empty()) dialog.set_current_folder(current);
    if (dialog.run() == Gtk::RESPONSE_OK) {
        entry->set_text(dialog.get_filename());
        save_settings();
    }
}

std::vector<std::string> RomManagerWindow::read_roms_paths() const {
    std::vector<std::string> paths;
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) {} }
    if (j.contains("roms_paths") && j["roms_paths"].is_array())
        for (const auto& p : j["roms_paths"])
            if (p.is_string()) paths.push_back(p.get<std::string>());
    return paths;
}

// ── Outbox tab ───────────────────────────────────────────────────────────────

// ── Quarantine tab ─────────────────────────────────────────────────────────────

void RomManagerWindow::build_quarantine_tab() {
    m_label_quarantine.set_text(_("Quarantine directory:"));
    m_label_quarantine.set_halign(Gtk::ALIGN_START);
    m_btn_browse_quarantine.set_label(_("Browse..."));
    m_entry_quarantine.set_hexpand(true);
    m_entry_quarantine.set_placeholder_text(
        _("Folder to move corrupt, unrepairable sets out of the library"));
    m_btn_browse_quarantine.signal_clicked().connect([this] {
        on_browse(&m_entry_quarantine);
        save_settings();
        refresh_quarantine_view();
    });
    m_quarantine_grid.set_column_spacing(8);
    m_quarantine_grid.attach(m_label_quarantine,      0, 0, 1, 1);
    m_quarantine_grid.attach(m_entry_quarantine,       1, 0, 1, 1);
    m_quarantine_grid.attach(m_btn_browse_quarantine, 2, 0, 1, 1);

    m_quarantine_model = Gtk::TreeStore::create(m_quarantine_cols);
    m_quarantine_view.set_model(m_quarantine_model);
    m_quarantine_view.append_column(_("System / Set"), m_quarantine_cols.name);
    m_quarantine_view.append_column(_("Sets"),         m_quarantine_cols.count);
    m_quarantine_view.append_column(_("Size"),         m_quarantine_cols.size);
    for (auto* c : m_quarantine_view.get_columns()) c->set_resizable(true);

    m_quarantine_scroll.add(m_quarantine_view);
    m_quarantine_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    m_quarantine_summary.set_halign(Gtk::ALIGN_START);
    m_quarantine_summary.set_text(_("Quarantine is empty."));

    m_btn_refresh_quarantine.signal_clicked().connect(
        sigc::mem_fun(*this, &RomManagerWindow::refresh_quarantine_view));
    m_btn_open_quarantine.signal_clicked().connect(
        sigc::mem_fun(*this, &RomManagerWindow::on_open_quarantine_clicked));
    m_btn_purge_quarantine.signal_clicked().connect(
        sigc::mem_fun(*this, &RomManagerWindow::on_purge_quarantine_clicked));
    m_btn_purge_quarantine.get_style_context()->add_class("destructive-action");

    m_quarantine_buttons.set_layout(Gtk::BUTTONBOX_END);
    m_quarantine_buttons.set_spacing(6);
    m_quarantine_buttons.pack_start(m_btn_refresh_quarantine);
    m_quarantine_buttons.pack_start(m_btn_open_quarantine);
    m_quarantine_buttons.pack_start(m_btn_purge_quarantine);

    m_quarantine_box.set_margin_start(10);
    m_quarantine_box.set_margin_end(10);
    m_quarantine_box.set_margin_top(10);
    m_quarantine_box.set_margin_bottom(10);
    m_quarantine_box.pack_start(m_quarantine_grid,    Gtk::PACK_SHRINK);
    m_quarantine_box.pack_start(m_quarantine_summary, Gtk::PACK_SHRINK);
    m_quarantine_box.pack_start(m_quarantine_scroll,  Gtk::PACK_EXPAND_WIDGET);
    m_quarantine_box.pack_start(m_quarantine_buttons, Gtk::PACK_SHRINK);
}

void RomManagerWindow::refresh_quarantine_view() {
    m_quarantine_model->clear();
    const std::string quarantine = m_entry_quarantine.get_text();
    std::error_code ec;
    if (quarantine.empty() || !fs::is_directory(quarantine, ec)) {
        m_quarantine_summary.set_text(_("Quarantine is empty."));
        return;
    }

    int total_sets = 0;
    uintmax_t total_bytes = 0;

    std::vector<fs::path> systems;
    for (auto it = fs::directory_iterator(quarantine, ec); it != fs::directory_iterator(); ++it)
        if (it->is_directory(ec)) systems.push_back(it->path());
    std::sort(systems.begin(), systems.end());

    for (const auto& sysdir : systems) {
        std::vector<fs::path> zips;
        for (auto it = fs::directory_iterator(sysdir, ec); it != fs::directory_iterator(); ++it)
            if (it->is_regular_file(ec) && it->path().extension() == ".zip")
                zips.push_back(it->path());
        if (zips.empty()) continue;
        std::sort(zips.begin(), zips.end());

        uintmax_t sys_bytes = 0;
        for (const auto& z : zips) sys_bytes += fs::file_size(z, ec);

        auto parent = *(m_quarantine_model->append());
        parent[m_quarantine_cols.name]  = sysdir.filename().string();
        parent[m_quarantine_cols.count] = std::to_string(zips.size());
        parent[m_quarantine_cols.size]  = human_size(sys_bytes);

        for (const auto& z : zips) {
            auto child = *(m_quarantine_model->append(parent.children()));
            child[m_quarantine_cols.name] = z.filename().string();
            child[m_quarantine_cols.size] = human_size(fs::file_size(z, ec));
        }

        total_sets += (int)zips.size();
        total_bytes += sys_bytes;
    }

    m_quarantine_summary.set_text(total_sets == 0
        ? Glib::ustring(_("Quarantine is empty."))
        : Glib::ustring::compose(_("%1 set(s) across %2 system(s) : %3"),
                                 total_sets, (int)m_quarantine_model->children().size(),
                                 human_size(total_bytes)));
}

void RomManagerWindow::on_open_quarantine_clicked() {
    const std::string quarantine = m_entry_quarantine.get_text();
    std::error_code ec;
    if (quarantine.empty() || !fs::is_directory(quarantine, ec)) return;
    try {
        Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(quarantine));
    } catch (const Glib::Error& e) {
        Gtk::MessageDialog dlg(*this, _("Could not open the folder."), false,
                               Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        dlg.set_secondary_text(e.what());
        dlg.run();
    }
}

void RomManagerWindow::on_purge_quarantine_clicked() {
    const std::string quarantine = m_entry_quarantine.get_text();
    std::error_code ec;
    if (quarantine.empty() || !fs::is_directory(quarantine, ec)) return;

    int count = 0;
    uintmax_t bytes = 0;
    std::vector<std::string> listed;
    for (auto it = fs::recursive_directory_iterator(quarantine, ec); it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file(ec) && !RomManifest::Manifest::is_manifest_file(it->path().filename().string())) {
            count++;
            bytes += fs::file_size(it->path(), ec);
            listed.push_back(it->path().string());
        }

    if (count == 0) {
        m_quarantine_summary.set_text(_("Quarantine is empty."));
        return;
    }

    ConfirmationDialog confirm(*this, _("Permanently delete quarantined ROMs?"),
        Glib::ustring::compose(
            _("%1 file(s) (%2) will be permanently deleted from your filesystem : not moved, "
              "not recoverable.\n\nThis cannot be undone."),
            count, human_size(bytes)),
        "🗑️", /*destructive=*/true);
    if (!confirm.show_and_confirm()) return;

    // A full manifest before an irreversible delete : the one thing that would
    // have settled the "did purge eat something real" question with certainty
    // instead of a guess, had it ever actually happened.
    push_log("[PURGE-QUARANTINE] deleting " + std::to_string(count) + " file(s), " + human_size(bytes) + ":");
    for (const auto& f : listed) push_log("[PURGE-QUARANTINE]   " + f);

    // The record of what was here outlives the files: every entry is retired
    // to the manifest's history as purged, and the manifest is the one file
    // written back into the emptied folder.
    RomManifest::Manifest manifest = RomManifest::Manifest::load(quarantine);
    manifest.reconcile();
    manifest.retire_all(RomManifest::outcome::Purged);

    fs::remove_all(quarantine, ec);
    fs::create_directories(quarantine, ec); // keep the configured path valid and empty
    manifest.save();

    refresh_quarantine_view();
    m_quarantine_summary.set_text(Glib::ustring::compose(
        _("Purged %1 file(s) (%2)."), count, human_size(bytes)));
}

// ── DAT tab ──────────────────────────────────────────────────────────────────

void RomManagerWindow::build_dat_tab() {
    m_label_dat.set_text(_("DAT directory:"));
    m_btn_browse_dat.set_label(_("Browse..."));
    m_btn_generate_dat.set_label(_("Generate DAT files from FBNeo"));
    m_btn_update_dat.set_label(_("Update database from DAT files"));

    m_dat_grid.set_column_spacing(8);
    m_dat_grid.set_row_spacing(6);
    m_label_dat.set_halign(Gtk::ALIGN_START);
    m_entry_dat.set_hexpand(true);

    m_btn_browse_dat.signal_clicked().connect([this] {
        on_browse(&m_entry_dat);
        m_sig_dat_path_changed.emit(m_entry_dat.get_text());
        refresh_dat_list();
    });
    m_entry_dat.signal_activate().connect([this] {
        save_settings();
        m_sig_dat_path_changed.emit(m_entry_dat.get_text());
        refresh_dat_list();
    });

    m_btn_generate_dat.signal_clicked().connect([this] {
        // Reuses the exact same helper the Settings panel drives, including its
        // success dialog which writes the resulting path back into the entry.
        // It wants the FBNeo *executable* : this window has no SettingsPanel to
        // ask, so the path comes straight from config.json.
        const std::string fbneo = fbneo_executable();
        if (fbneo.empty()) {
            Gtk::MessageDialog dlg(*this, _("No FBNeo executable configured."),
                                   false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
            dlg.set_secondary_text(_("Set it in Settings before generating DAT files."));
            dlg.run();
            return;
        }
        GenerateDAT::execute(*this, fbneo, m_entry_dat.get_text(), &m_entry_dat);
        save_settings();
        m_sig_dat_path_changed.emit(m_entry_dat.get_text());
        refresh_dat_list();
    });
    m_btn_update_dat.signal_clicked().connect([this] { m_sig_update_dat.emit(); });
    m_btn_update_dat.get_style_context()->add_class("accent-button");

    m_dat_grid.attach(m_label_dat,      0, 0, 1, 1);
    m_dat_grid.attach(m_entry_dat,      1, 0, 1, 1);
    m_dat_grid.attach(m_btn_browse_dat, 2, 0, 1, 1);

    m_dat_model = Gtk::ListStore::create(m_dat_cols);
    m_dat_view.set_model(m_dat_model);
    m_dat_view.append_column(_("DAT file"), m_dat_cols.filename);
    m_dat_view.append_column(_("Games"),    m_dat_cols.games);
    m_dat_view.append_column(_("Imported"), m_dat_cols.modified);
    for (auto* c : m_dat_view.get_columns()) c->set_resizable(true);
    m_dat_scroll.add(m_dat_view);
    m_dat_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    auto* buttons = Gtk::make_managed<Gtk::ButtonBox>(Gtk::ORIENTATION_HORIZONTAL);
    buttons->set_layout(Gtk::BUTTONBOX_END);
    buttons->set_spacing(6);
    buttons->pack_start(m_btn_generate_dat);
    buttons->pack_start(m_btn_update_dat);

    m_dat_box.set_margin_start(10);
    m_dat_box.set_margin_end(10);
    m_dat_box.set_margin_top(10);
    m_dat_box.set_margin_bottom(10);
    m_dat_box.pack_start(m_dat_grid,   Gtk::PACK_SHRINK);
    m_dat_box.pack_start(m_dat_scroll, Gtk::PACK_EXPAND_WIDGET);
    m_dat_box.pack_start(*buttons,     Gtk::PACK_SHRINK);
}

void RomManagerWindow::refresh_dat_list() {
    m_dat_model->clear();
    const std::string dir = m_entry_dat.get_text();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;

    std::vector<fs::path> dats;
    for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".dat") dats.push_back(it->path());
    }
    std::sort(dats.begin(), dats.end());

    for (const auto& d : dats) {
        auto row = *(m_dat_model->append());
        row[m_dat_cols.filename] = d.filename().string();
        row[m_dat_cols.games]    = "";
        row[m_dat_cols.modified] = human_size(fs::file_size(d, ec));
    }
}

// ── Settings persistence ─────────────────────────────────────────────────────

std::string RomManagerWindow::fbneo_executable() const {
    // Owned by the Settings panel; read fresh rather than cached so a change made
    // there while this window stayed open is picked up.
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { return {}; } }
    if (j.contains("fbneo_executable") && j["fbneo_executable"].is_string())
        return j["fbneo_executable"].get<std::string>();
    return {};
}

void RomManagerWindow::reload_settings() {
    nlohmann::json j;
    {
        std::ifstream fi(AppContext::get_config_path());
        if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    }
    if (j.contains("dat_path") && j["dat_path"].is_string())
        m_entry_dat.set_text(j["dat_path"].get<std::string>());

    if (j.contains("rom_manager") && j["rom_manager"].is_object()) {
        const auto& rm = j["rom_manager"];
        if (rm.contains("quarantine_path") && rm["quarantine_path"].is_string())
            m_entry_quarantine.set_text(rm["quarantine_path"].get<std::string>());
    }

    if (m_import) m_import->reload_settings();
    if (m_outbox) m_outbox->reload_settings();
    refresh_quarantine_view();
    refresh_dat_list();
}

void RomManagerWindow::refresh_after_scan() {
    if (m_outbox) m_outbox->refresh();
    refresh_quarantine_view();
    if (m_library) m_library->refresh_after_scan();
}

void RomManagerWindow::save_settings() {
    // Read-modify-write, like every other config.json writer in the app, so keys
    // owned by the Settings panel and the controller manager survive.
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    {
        std::ifstream fi(path);
        if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    }
    j["rom_manager"]["quarantine_path"] = m_entry_quarantine.get_text().raw();
    j["dat_path"]                       = m_entry_dat.get_text().raw();

    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}
