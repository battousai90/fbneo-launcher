// src/RomManagerWindow.cpp
#include "RomManagerWindow.h"
#include "RomArchive.h"
#include "RomLibraryTab.h"
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
    close->signal_clicked().connect([this] { if (!m_busy && !(m_library && m_library->busy())) hide(); });

    m_headerbar.set_show_close_button(false);
    m_headerbar.pack_start(*head);
    m_headerbar.pack_end(*close);
    m_headerbar.set_custom_title(*Gtk::make_managed<Gtk::Box>());
    m_headerbar.show_all();
    set_titlebar(m_headerbar);

    // ── Tabs ──────────────────────────────────────────────────────────────
    m_library = Gtk::make_managed<RomLibraryTab>(m_db, [this] {
        RomLibraryTab::Paths p;
        p.roms_paths = read_roms_paths();
        p.inbox      = m_entry_inbox.get_text().raw();
        p.quarantine = m_entry_quarantine.get_text().raw();
        return p;
    });
    m_library->signal_rescan_requested().connect([this] { m_sig_rescan_requested.emit(); });
    m_library->signal_scan_requested().connect([this] { m_sig_scan_requested.emit(); });
    m_library->signal_log().connect([this](std::string line) { push_log(line); });
    m_library->signal_send_to_import().connect(sigc::mem_fun(*this, &RomManagerWindow::on_send_to_import));

    build_import_tab();
    build_outbox_tab();
    build_quarantine_tab();
    build_dat_tab();

    m_tabbar.get_style_context()->add_class("set-tabbar");
    m_pages.set_transition_type(Gtk::STACK_TRANSITION_TYPE_NONE);
    m_pages.add(*m_library,       "library");
    m_pages.add(m_import_box,     "import");
    m_pages.add(m_outbox_box,     "outbox");
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

    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomManagerWindow::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &RomManagerWindow::on_worker_finished));

    reload_settings();
    show_all_children();
    m_infobar.hide();
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

// Library hands over the archives of sets it found repairable: copied (never
// moved : the library is read-only from here) into the inbox, then Import
// takes the stage and analyses them straight away.
void RomManagerWindow::on_send_to_import(std::vector<std::string> archives) {
    if (m_busy) return;
    const std::string inbox = m_entry_inbox.get_text();
    std::error_code ec;
    if (inbox.empty()) {
        SettingsUi::notice(*this, _("No import folder"), _("Set an import folder in the Import tab first."));
        return;
    }
    fs::create_directories(inbox, ec);
    int copied = 0, skipped = 0;
    for (const auto& a : archives) {
        fs::path src(a);
        fs::path dest = fs::path(inbox) / src.filename();
        if (fs::exists(dest, ec)) { ++skipped; continue; }
        fs::copy_file(src, dest, ec);
        if (ec) { push_log("[SEND-TO-IMPORT] could not copy " + a + ": " + ec.message()); ec.clear(); ++skipped; }
        else    { ++copied; push_log("[SEND-TO-IMPORT] " + src.filename().string() + " -> inbox"); }
    }
    show_tab("import");
    m_summary_label.set_text(Glib::ustring::compose(
        _("%1 set(s) copied from the library into the import folder (%2 already there). Analysing…"), copied, skipped));
    on_analyze_clicked();
}

RomManagerWindow::~RomManagerWindow() {
    if (m_worker.joinable()) {
        m_cancelled = true;
        m_worker.join();
    }
}

// ── Import tab ───────────────────────────────────────────────────────────────

void RomManagerWindow::build_import_tab() {
    m_paths_grid.set_column_spacing(8);
    m_paths_grid.set_row_spacing(6);

    // Labels declared with English defaults in the header are re-set here so they
    // go through the catalogue like the rest of the tab.
    m_label_inbox.set_text(_("Inbox:"));
    m_check_recursive.set_label(_("Scan the inbox recursively"));
    m_btn_browse_inbox.set_label(_("Browse..."));
    m_btn_analyze.set_label(_("Analyse"));
    m_btn_fix.set_label(_("Fix"));
    m_btn_cancel.set_label(_("Cancel"));
    m_btn_close.set_label(_("Close"));

    m_label_inbox.set_halign(Gtk::ALIGN_START);
    m_entry_inbox.set_hexpand(true);
    m_entry_inbox.set_placeholder_text(_("Folder where you drop downloaded archives (zip, 7z, rar…)"));

    m_btn_browse_inbox.signal_clicked().connect(
        [this] { on_browse(&m_entry_inbox); });

    m_paths_grid.attach(m_label_inbox,      0, 0, 1, 1);
    m_paths_grid.attach(m_entry_inbox,      1, 0, 1, 1);
    m_paths_grid.attach(m_btn_browse_inbox, 2, 0, 1, 1);
    m_paths_grid.attach(m_check_recursive,   1, 1, 2, 1);

    m_infobar.set_message_type(Gtk::MESSAGE_WARNING);
    m_infobar.set_no_show_all(true);
    dynamic_cast<Gtk::Container*>(m_infobar.get_content_area())->add(m_infobar_label);
    m_infobar_label.show();

    m_progress.set_show_text(true);
    m_progress.set_text("0%");
    m_current_label.set_halign(Gtk::ALIGN_START);
    m_current_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_current_label.set_text(_("Idle."));

    // ── Filter row ───────────────────────────────────────────────────────────
    m_combo_filter.append("all",        _("All"));
    m_combo_filter.append("actionable", _("Fixable only"));
    m_combo_filter.append("missing",    _("Incomplete only"));
    m_combo_filter.append("unknown",    _("Unrecognized only"));
    m_combo_filter.set_active_id("all");
    m_combo_filter.signal_changed().connect(sigc::mem_fun(*this, &RomManagerWindow::on_filter_changed));

    m_btn_select_all.set_label(_("Select all"));
    m_btn_select_none.set_label(_("Select none"));
    m_btn_export.set_label(_("Export missing list..."));
    m_btn_select_all.signal_clicked().connect([this] { set_all_selected(true); });
    m_btn_select_none.signal_clicked().connect([this] { set_all_selected(false); });
    m_btn_export.signal_clicked().connect(sigc::mem_fun(*this, &RomManagerWindow::on_export_missing));

    auto* flabel = Gtk::make_managed<Gtk::Label>(_("Show:"));
    m_filter_box.pack_start(*flabel,           Gtk::PACK_SHRINK);
    m_filter_box.pack_start(m_combo_filter,    Gtk::PACK_SHRINK);
    m_filter_box.pack_start(m_btn_select_all,  Gtk::PACK_SHRINK);
    m_filter_box.pack_start(m_btn_select_none, Gtk::PACK_SHRINK);
    m_filter_box.pack_end(m_btn_export,        Gtk::PACK_SHRINK);

    build_stats_bar();

    // ── Results tree ─────────────────────────────────────────────────────────
    // Filter built after the fill, not here : see populate_results().
    m_results_model = Gtk::TreeStore::create(m_cols);
    m_results_view.set_enable_tree_lines(true);

    auto* toggle = Gtk::make_managed<Gtk::CellRendererToggle>();
    toggle->set_activatable(true);
    toggle->signal_toggled().connect(sigc::mem_fun(*this, &RomManagerWindow::on_row_toggled));
    int col = m_results_view.append_column(_("Fix"), *toggle) - 1;
    if (auto* c = m_results_view.get_column(col)) {
        c->add_attribute(toggle->property_active(), m_cols.include);
        // Non-actionable rows (incomplete / already owned) show a greyed-out box
        // instead of pretending they can be selected; ROM detail rows show none.
        c->add_attribute(toggle->property_activatable(), m_cols.actionable);
        c->add_attribute(toggle->property_sensitive(),   m_cols.actionable);
        c->add_attribute(toggle->property_visible(),     m_cols.is_set);
    }
    m_results_view.append_column(_("Game / ROM"),  m_cols.game);
    m_results_view.append_column(_("System"),      m_cols.system);
    m_results_view.append_column(_("Status"),      m_cols.action);
    m_results_view.append_column(_("Destination"), m_cols.destination);
    m_results_view.append_column(_("Details"),     m_cols.details);
    m_results_view.append_column(_("CRC"),         m_cols.crc);

    // Colour the status text per row, RomVault style.
    if (auto* c = m_results_view.get_column(3))
        if (auto* r = dynamic_cast<Gtk::CellRendererText*>(m_results_view.get_column_cell_renderer(3))) {
            c->add_attribute(r->property_foreground(), m_cols.colour);
            r->property_weight() = Pango::WEIGHT_BOLD;
        }

    for (auto* c : m_results_view.get_columns()) { c->set_resizable(true); c->set_expand(false); }
    if (auto* c = m_results_view.get_column(1)) { c->set_expand(true); c->set_min_width(220); }
    if (auto* c = m_results_view.get_column(5)) c->set_expand(true);
    m_results_view.set_expander_column(*m_results_view.get_column(1));

    m_results_scroll.add(m_results_view);
    m_results_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_results_scroll.set_size_request(-1, 300);

    m_summary_label.set_halign(Gtk::ALIGN_START);
    m_summary_label.set_text(_("No analysis yet."));

    m_log_buffer = Gtk::TextBuffer::create();
    m_log_view.set_buffer(m_log_buffer);
    m_log_view.set_editable(false);
    m_log_view.set_cursor_visible(false);
    m_log_view.set_monospace(true);
    m_log_scroll.add(m_log_view);
    m_log_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_log_scroll.set_size_request(-1, 130);

    m_btn_analyze.signal_clicked().connect(sigc::mem_fun(*this, &RomManagerWindow::on_analyze_clicked));
    m_btn_fix.signal_clicked().connect(sigc::mem_fun(*this, &RomManagerWindow::on_fix_clicked));
    m_btn_cancel.signal_clicked().connect(sigc::mem_fun(*this, &RomManagerWindow::on_cancel_clicked));
    m_btn_close.signal_clicked().connect([this] { hide(); });
    m_btn_fix.set_sensitive(false);
    m_btn_cancel.set_sensitive(false);
    m_btn_analyze.get_style_context()->add_class("accent-button");

    m_import_buttons.set_layout(Gtk::BUTTONBOX_END);
    m_import_buttons.set_spacing(6);
    m_import_buttons.pack_start(m_btn_analyze);
    m_import_buttons.pack_start(m_btn_fix);
    m_import_buttons.pack_start(m_btn_cancel);
    m_import_buttons.pack_start(m_btn_close);

    m_import_box.set_margin_start(10);
    m_import_box.set_margin_end(10);
    m_import_box.set_margin_top(10);
    m_import_box.set_margin_bottom(10);
    m_import_box.pack_start(m_paths_grid,     Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_infobar,        Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_current_label,  Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_progress,       Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_stats_box,      Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_filter_box,     Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_results_scroll, Gtk::PACK_EXPAND_WIDGET);
    m_import_box.pack_start(m_summary_label,  Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_log_scroll,     Gtk::PACK_SHRINK);
    m_import_box.pack_start(m_import_buttons, Gtk::PACK_SHRINK);
}

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

void RomManagerWindow::on_row_toggled(const Glib::ustring& path) {
    // The view is backed by the filter model, so the path must be converted before
    // it can address the underlying store.
    if (!m_results_filter) return;
    auto fit = m_results_filter->get_iter(path);
    if (!fit) return;
    auto it = m_results_filter->convert_iter_to_child_iter(fit);
    if (!it) return;
    if (!(*it)[m_cols.is_set] || !(*it)[m_cols.actionable]) return;
    bool now = !(*it)[m_cols.include];
    (*it)[m_cols.include] = now;
    m_report.sets[(*it)[m_cols.index]].selected = now;
    update_summary();
}

void RomManagerWindow::set_all_selected(bool on) {
    for (auto& row : m_results_model->children()) {
        if (!row[m_cols.is_set] || !row[m_cols.actionable]) continue;
        row[m_cols.include] = on;
        m_report.sets[row[m_cols.index]].selected = on;
    }
    update_summary();
}

// ── Status presentation ──────────────────────────────────────────────────────

namespace {

struct StatusStyle { const char* label; const char* colour; };

// RomVault's vocabulary and colour language: green = correct, amber = fixable,
// red = missing, grey = nothing to do.
StatusStyle style_for(int kind) {
    switch (kind) {
        case RomManagerWindow::KIND_MOVE:        return {"Correct",    "#3fb950"};
        case RomManagerWindow::KIND_REBUILD:     return {"Fixable",    "#d29922"};
        case RomManagerWindow::KIND_INCOMPLETE:  return {"Missing",    "#f85149"};
        case RomManagerWindow::KIND_IN_LIBRARY:  return {"In library", "#8b949e"};
        case RomManagerWindow::KIND_UNKNOWN:     return {"Unknown",    "#a371f7"};
        case RomManagerWindow::KIND_MISSING_ROM: return {"Missing",    "#f85149"};
        case RomManagerWindow::KIND_SOURCED_ROM: return {"Fixable",    "#d29922"};
    }
    return {"?", "#8b949e"};
}

int kind_for(RomInbox::Action a) {
    switch (a) {
        case RomInbox::Action::Move:             return RomManagerWindow::KIND_MOVE;
        case RomInbox::Action::Rebuild:          return RomManagerWindow::KIND_REBUILD;
        case RomInbox::Action::Incomplete:       return RomManagerWindow::KIND_INCOMPLETE;
        case RomInbox::Action::AlreadyInLibrary: return RomManagerWindow::KIND_IN_LIBRARY;
    }
    return RomManagerWindow::KIND_UNKNOWN;
}

std::string crc_hex(unsigned long crc) {
    std::ostringstream os;
    os << std::hex << std::setw(8) << std::setfill('0') << crc;
    return os.str();
}

} // namespace

void RomManagerWindow::build_stats_bar() {
    struct { Gtk::Label* w; const char* colour; } pills[] = {
        {&m_stat_complete, "#3fb950"}, {&m_stat_fixable, "#d29922"},
        {&m_stat_missing,  "#f85149"}, {&m_stat_library, "#8b949e"},
        {&m_stat_unknown,  "#a371f7"}, {&m_stat_selected, "#58a6ff"},
    };
    for (auto& p : pills) {
        p.w->set_use_markup(true);
        p.w->set_halign(Gtk::ALIGN_START);
        m_stats_box.pack_start(*p.w, Gtk::PACK_SHRINK);
    }
    m_stats_box.set_margin_top(4);
    update_stats();
}

void RomManagerWindow::update_stats() {
    int selected = 0;
    for (const auto& s : m_report.sets) if (s.selected) ++selected;

    auto pill = [](Gtk::Label& w, const char* colour, const std::string& text, int n) {
        w.set_markup("<span background='" + std::string(colour) + "' foreground='#0d1117'"
                     " weight='bold'> " + Glib::Markup::escape_text(text) + " " +
                     std::to_string(n) + " </span>");
    };
    pill(m_stat_complete, "#3fb950", _("Correct"),    m_report.complete);
    pill(m_stat_fixable,  "#d29922", _("Fixable"),    m_report.fixable);
    pill(m_stat_missing,  "#f85149", _("Missing"),    m_report.incomplete);
    pill(m_stat_library,  "#8b949e", _("In library"), m_report.already + (int)m_report.already_have.size());
    pill(m_stat_unknown,  "#a371f7", _("Unknown"),    (int)m_report.unrecognized.size());
    pill(m_stat_selected, "#58a6ff", _("Selected"),   selected);
}

bool RomManagerWindow::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const auto& row = *it;
    if (!row[m_cols.is_set]) return true;  // detail rows follow their parent
    Glib::ustring mode = m_combo_filter.get_active_id();
    if (mode.empty() || mode == "all") return true;
    int kind = row[m_cols.kind];
    if (mode == "actionable") return kind == KIND_MOVE || kind == KIND_REBUILD;
    if (mode == "missing")    return kind == KIND_INCOMPLETE;
    if (mode == "unknown")    return kind == KIND_UNKNOWN;
    return true;
}

void RomManagerWindow::on_filter_changed() {
    if (m_results_filter) m_results_filter->refilter();
}

void RomManagerWindow::append_rom_children(const Gtk::TreeModel::Row& parent,
                                           const RomInbox::SetPlan& s) {
    // Every ROM the set is still short of : the list the user needs to go hunting.
    for (const auto& m : s.missing) {
        auto r = *(m_results_model->append(parent.children()));
        auto st = style_for(KIND_MISSING_ROM);
        r[m_cols.is_set]  = false;
        r[m_cols.kind]    = KIND_MISSING_ROM;
        r[m_cols.game]    = m.name;
        r[m_cols.action]  = _(st.label);
        r[m_cols.colour]  = st.colour;
        r[m_cols.details] = Glib::ustring::compose("CRC %1 · %2", crc_hex(m.crc), human_size(m.size));
        r[m_cols.crc]     = crc_hex(m.crc);
    }
    // For a rebuild, spell out every piece that is not simply already in place:
    // borrowed from the library, taken from another inbox archive, or renamed.
    if (s.action == RomInbox::Action::Rebuild) {
        for (const auto& p : s.pieces) {
            if (!p.resolved) continue;
            bool same_archive = (p.src.container == s.trigger_archive);
            bool same_name    = (p.src.entry == p.target_name);
            if (same_archive && same_name) continue;
            auto r = *(m_results_model->append(parent.children()));
            auto st = style_for(KIND_SOURCED_ROM);
            r[m_cols.is_set] = false;
            r[m_cols.kind]   = KIND_SOURCED_ROM;
            r[m_cols.game]   = p.target_name;
            r[m_cols.action] = _(st.label);
            r[m_cols.colour] = st.colour;
            std::string origin = same_archive
                ? Glib::ustring::compose(_("renamed from %1"), p.src.entry).raw()
                : Glib::ustring::compose(_("from %1"),
                      fs::path(p.src.container).filename().string()).raw();
            if (!same_archive && !same_name)
                origin += Glib::ustring::compose(_(" (as %1)"), p.src.entry).raw();
            r[m_cols.details] = origin;
            r[m_cols.crc]     = crc_hex(p.crc);
        }
    }
}

// ── Worker plumbing ──────────────────────────────────────────────────────────

void RomManagerWindow::push_progress(double pct, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_current_message = msg;
    }
    m_progress_value.store(pct);

    // Throttle: only wake the main loop every UI_DISPATCH_INTERVAL_MS.
    static thread_local auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= UI_DISPATCH_INTERVAL_MS) {
        last = now;
        m_progress_dispatcher();
    }
}

void RomManagerWindow::push_log(const std::string& msg) {
    // main.cpp redirects std::cerr into debug.log, so this is the one line
    // that turns every RomInbox/RomAudit/RomCleanup log message from "gone the
    // moment this dialog closes" into something that survives to be read
    // after the fact : the exact gap that made the Move-to-library and Import
    // "Unknown" investigations dead ends earlier.
    std::cerr << "[ROM-MANAGER] " << msg << std::endl;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.push_back(msg);
    }
    m_progress_dispatcher();  // safe from any thread
}

RomInbox::Callbacks RomManagerWindow::make_callbacks() {
    RomInbox::Callbacks cb;
    cb.progress  = [this](double p, const std::string& m) { push_progress(p, m); };
    cb.log       = [this](const std::string& m) { push_log(m); };
    cb.cancelled = [this] { return m_cancelled.load(); };
    return cb;
}

void RomManagerWindow::on_progress_update() {
    std::string message;
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        message = m_current_message;
        pending.swap(m_log_messages);
    }
    double pct = m_progress_value.load();
    m_progress.set_fraction(std::clamp(pct / 100.0, 0.0, 1.0));
    m_progress.set_text(std::to_string((int)pct) + "%");
    if (!message.empty()) m_current_label.set_text(message);
    for (const auto& l : pending)
        m_log_buffer->insert(m_log_buffer->end(), l + "\n");
    if (!pending.empty()) m_log_view.scroll_to(m_log_buffer->get_insert());
}

void RomManagerWindow::on_worker_finished() {
    if (m_worker.joinable()) m_worker.join();

    on_progress_update();  // flush whatever the worker logged last

    if (m_job == Job::Analyze) {
        populate_results();
        if (m_report.library_pool_empty) {
            m_infobar_label.set_text(
                _("The ROM library index is empty : run a ROM scan first, otherwise sets that could be rebuilt will be reported as incomplete."));
            m_infobar.show();
        } else {
            m_infobar.hide();
        }
    } else if (m_job == Job::Apply) {
        for (const auto& e : m_apply_result.errors)
            m_log_buffer->insert(m_log_buffer->end(), "  ! " + e + "\n");
        refresh_outbox_view();
        // The plan is stale now that files have moved: force a fresh analysis.
        m_results_model->clear();
        m_report = RomInbox::Report{};
        m_summary_label.set_text(
            Glib::ustring::compose(_("Fix done: %1 moved, %2 rebuilt, %3 failed. Re-run Analyse to refresh."),
                                   m_apply_result.moved, m_apply_result.rebuilt, m_apply_result.failed));
    }

    m_job = Job::None;
    set_busy(false);
}

void RomManagerWindow::set_busy(bool busy) {
    m_busy = busy;
    m_btn_analyze.set_sensitive(!busy);
    m_btn_cancel.set_sensitive(busy);
    m_btn_close.set_sensitive(!busy);
    m_entry_inbox.set_sensitive(!busy);
    m_entry_outbox.set_sensitive(!busy);
    m_btn_browse_inbox.set_sensitive(!busy);
    m_btn_browse_outbox.set_sensitive(!busy);
    m_check_recursive.set_sensitive(!busy);
    if (busy) m_btn_fix.set_sensitive(false);
    else      update_summary();
}

void RomManagerWindow::on_cancel_clicked() {
    m_cancelled = true;
    m_current_label.set_text(_("Cancelling…"));
}

// ── Analyse ──────────────────────────────────────────────────────────────────

void RomManagerWindow::on_analyze_clicked() {
    if (m_busy) return;

    const std::string inbox  = m_entry_inbox.get_text();
    const std::string outbox = m_entry_outbox.get_text();

    std::error_code ec;
    if (inbox.empty() || !fs::is_directory(inbox, ec)) {
        Gtk::MessageDialog dlg(*this, _("Select a valid inbox folder first."),
                               false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        dlg.run();
        return;
    }
    if (outbox.empty()) {
        Gtk::MessageDialog dlg(*this, _("Select an outbox folder first."),
                               false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        dlg.run();
        return;
    }
    if (fs::equivalent(inbox, outbox, ec)) {
        Gtk::MessageDialog dlg(*this, _("The inbox and the outbox must be two different folders."),
                               false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        dlg.run();
        return;
    }

    save_settings();
    m_log_buffer->set_text("");
    m_results_model->clear();
    m_report = RomInbox::Report{};
    m_cancelled = false;

    // Snapshot the widget values here, on the GTK main thread.
    m_job_inbox     = inbox;
    m_job_outbox    = outbox;
    m_job_recursive = m_check_recursive.get_active();
    m_job_roms_paths = read_roms_paths();

    m_job = Job::Analyze;
    set_busy(true);
    m_worker = std::thread(&RomManagerWindow::worker_analyze, this);
}

void RomManagerWindow::worker_analyze() {
    RomInbox::Callbacks cb = make_callbacks();

    // The library is never written to directly (see the class comment), but a
    // set the Library tab already knows is wrong : right data, wrong entry
    // name, nothing missing : shouldn't need the user to go hunt its archive
    // down by hand. Pull those in here, so Analyse alone is enough: audit,
    // then copy each repairable archive into the inbox before scanning it.
    cb.log("Checking the library for sets the audit can already fix on its own…");
    RomAudit::Report audit = RomAudit::audit(m_db, m_job_roms_paths, /*problems_only=*/true, cb);
    int pulled = 0;
    for (const auto& g : audit.games) {
        if (cb.cancelled()) break;
        if (!g.repairable || !g.archive_found || g.archive.empty()) continue;

        fs::path src(g.archive);
        fs::path dest = fs::path(m_job_inbox) / src.filename();
        std::error_code ec;
        if (fs::exists(dest, ec)) continue;   // already pulled in by a previous run

        fs::copy_file(src, dest, ec);
        if (ec) {
            cb.log("  ⚠ could not copy " + src.filename().string() + " from the library: " + ec.message());
        } else {
            cb.log("  pulled " + src.filename().string() + " from the library (wrong entry name)");
            pulled++;
        }
    }
    if (pulled > 0)
        cb.log(std::to_string(pulled) + " set(s) pulled from the library into the inbox.");

    // Reads only the snapshot taken on the main thread : never a widget.
    m_report = RomInbox::analyze(m_job_inbox, m_job_outbox, m_db,
                                 m_job_recursive, cb);
    m_finished_dispatcher();
}

void RomManagerWindow::populate_results() {
    // Same reason as populate_audit(): filling a model that a view and a filter are
    // both watching makes a large inbox take minutes instead of milliseconds.
    m_results_view.unset_model();
    m_results_filter.reset();
    m_results_model->clear();

    for (unsigned int i = 0; i < m_report.sets.size(); ++i) {
        const auto& s = m_report.sets[i];
        bool actionable = (s.action == RomInbox::Action::Move ||
                           s.action == RomInbox::Action::Rebuild);
        int  kind = kind_for(s.action);
        auto st   = style_for(kind);

        auto row = *(m_results_model->append());
        row[m_cols.include]     = s.selected && actionable;
        row[m_cols.actionable]  = actionable;
        row[m_cols.is_set]      = true;
        row[m_cols.kind]        = kind;
        row[m_cols.game]        = s.description.empty() ? s.game_name : s.description;
        row[m_cols.system]      = s.system;
        row[m_cols.action]      = _(st.label);
        row[m_cols.colour]      = st.colour;
        row[m_cols.destination] = actionable ? s.dat_header : "";
        row[m_cols.index]       = i;

        std::string details;
        switch (s.action) {
            case RomInbox::Action::Incomplete:
                details = Glib::ustring::compose(_("%1 of %2 ROM(s) missing"),
                                                 (int)s.missing.size(), (int)s.pieces.size());
                break;
            case RomInbox::Action::AlreadyInLibrary:
                details = _("already present in the ROM library");
                break;
            case RomInbox::Action::Move:
                details = fs::path(s.trigger_archive).filename().string();
                break;
            case RomInbox::Action::Rebuild: {
                std::vector<std::string> bits;
                if (s.pieces_from_library)
                    bits.push_back(Glib::ustring::compose(_("%1 from library"), s.pieces_from_library));
                if (s.renamed_entries)
                    bits.push_back(Glib::ustring::compose(_("%1 renamed"), s.renamed_entries));
                if (!s.extra_entries.empty())
                    bits.push_back(Glib::ustring::compose(_("%1 extra dropped"), (int)s.extra_entries.size()));
                details = join_preview(bits, 3);
                break;
            }
        }
        row[m_cols.details] = details;

        append_rom_children(row, s);
    }

    // Archives that matched no DAT entry get their own rows, so nothing in the
    // inbox is silently absent from the report.
    auto add_orphan = [&](const std::string& path, const char* why, ResultKind kind = KIND_UNKNOWN) {
        auto st = style_for(kind);
        auto row = *(m_results_model->append());
        row[m_cols.include]    = false;
        row[m_cols.actionable] = false;
        row[m_cols.is_set]     = true;
        row[m_cols.kind]       = kind;
        row[m_cols.game]       = fs::path(path).filename().string();
        row[m_cols.action]     = _(st.label);
        row[m_cols.colour]     = st.colour;
        row[m_cols.details]    = _(why);

        // Show exactly what was hashed : the single most useful thing for
        // figuring out *why* nothing matched (wrong CRC vs. genuinely absent
        // from the DAT), without having to re-open the archive by hand.
        std::vector<RomArchive::Entry> entries;
        if (RomArchive::read_entries(path, entries) && !entries.empty()) {
            std::vector<std::string> crcs;
            for (const auto& e : entries) crcs.push_back(crc_hex((unsigned long)e.crc));
            row[m_cols.crc] = join_preview(crcs, 4);
        }
    };
    for (const auto& p : m_report.unrecognized) add_orphan(p, N_("no DAT entry matches this archive"));
    for (const auto& p : m_report.unsupported)  add_orphan(p, N_("could not be read (corrupt or unsupported format)"));
    // Recognized only by content (not by this archive's own filename), and the
    // library already has that exact game complete and correct elsewhere // this file is a redundant duplicate, not an unrecognized one.
    for (const auto& p : m_report.already_have)
        add_orphan(p, N_("content matches a game already complete in your library, under a different filename"),
                   KIND_IN_LIBRARY);

    m_results_filter = Gtk::TreeModelFilter::create(m_results_model);
    m_results_filter->set_visible_func(sigc::mem_fun(*this, &RomManagerWindow::row_visible));
    m_results_view.set_model(m_results_filter);
    update_summary();
}

void RomManagerWindow::update_summary() {
    update_stats();

    if (m_report.sets.empty() && m_report.unrecognized.empty() && m_report.unsupported.empty()
        && m_report.already_have.empty()) {
        m_btn_fix.set_sensitive(false);
        m_summary_label.set_text(_("No analysis yet."));
        return;
    }
    int selected = 0, missing_roms = 0;
    for (const auto& s : m_report.sets) {
        if (s.selected) ++selected;
        missing_roms += (int)s.missing.size();
    }

    m_summary_label.set_text(Glib::ustring::compose(
        _("%1 set(s) · %2 ROM(s) missing in total · %3 unsupported archive(s)"),
        (int)m_report.sets.size(), missing_roms, (int)m_report.unsupported.size()));

    m_btn_fix.set_sensitive(!m_busy && selected > 0);
    m_btn_export.set_sensitive(missing_roms > 0);
}

// ── Missing-list export ──────────────────────────────────────────────────────

void RomManagerWindow::on_export_missing() {
    Gtk::FileChooserDialog dlg(*this, _("Export missing list"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Save"),   Gtk::RESPONSE_OK);
    dlg.set_current_name("missing-roms.txt");
    dlg.set_do_overwrite_confirmation(true);
    if (dlg.run() != Gtk::RESPONSE_OK) return;

    std::ofstream out(dlg.get_filename());
    if (!out) {
        Gtk::MessageDialog err(*this, _("Could not write the file."), false,
                               Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        err.run();
        return;
    }

    out << "# Missing ROMs : fbneo-launcher\n";
    out << "# inbox: " << m_job_inbox << "\n\n";
    std::string current_system;
    int total = 0;
    for (const auto& s : m_report.sets) {
        if (s.missing.empty()) continue;
        if (s.system != current_system) {
            current_system = s.system;
            out << "\n[" << current_system << "]\n";
        }
        out << "  " << s.game_name;
        if (!s.description.empty()) out << "  (" << s.description << ")";
        out << "\n";
        for (const auto& m : s.missing) {
            out << "      " << m.name << "\tcrc=" << crc_hex(m.crc)
                << "\tsize=" << m.size << "\n";
            ++total;
        }
    }
    out << "\n# " << total << " missing ROM(s)\n";
    out.close();

    Gtk::MessageDialog ok(*this, Glib::ustring::compose(_("%1 missing ROM(s) exported."), total),
                          false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
    ok.set_secondary_text(dlg.get_filename());
    ok.run();
}

// ── Fix ──────────────────────────────────────────────────────────────────────

void RomManagerWindow::on_fix_clicked() {
    if (m_busy) return;

    int moves = 0, rebuilds = 0;
    for (const auto& s : m_report.sets) {
        if (!s.selected) continue;
        if (s.action == RomInbox::Action::Move)    ++moves;
        if (s.action == RomInbox::Action::Rebuild) ++rebuilds;
    }
    if (moves + rebuilds == 0) return;

    // Files leave the inbox, so this is confirmed even though the ROM library is
    // never touched.
    std::string msg = Glib::ustring::compose(
        _("%1 set(s) will be moved and %2 rebuilt into:\n%3\n\n"
          "Source archives fully consumed will be removed from the inbox.\n"
          "Your existing ROM directories are not modified."),
        moves, rebuilds, m_entry_outbox.get_text());

    ConfirmationDialog confirm(*this, _("Apply fixes?"), msg, "📦");
    if (!confirm.show_and_confirm()) return;

    m_cancelled = false;
    m_job = Job::Apply;
    set_busy(true);
    m_worker = std::thread(&RomManagerWindow::worker_apply, this);
}

void RomManagerWindow::worker_apply() {
    m_apply_result = RomInbox::apply(m_report, make_callbacks());
    m_finished_dispatcher();
}

// ── Library tab ──────────────────────────────────────────────────────────────

namespace {

StatusStyle audit_game_style(const std::string& status) {
    if (status == "available") return {"Correct",   "#3fb950"};
    if (status == "incorrect") return {"Incorrect", "#d29922"};
    return {"Missing", "#f85149"};
}

StatusStyle audit_rom_style(RomAudit::RomState s) {
    switch (s) {
        case RomAudit::RomState::Present:   return {"OK",         "#3fb950"};
        case RomAudit::RomState::WrongName: return {"Wrong name", "#d29922"};
        case RomAudit::RomState::Corrupt:   return {"Corrupt",    "#db6d28"};
        case RomAudit::RomState::Absent:    return {"Absent",     "#f85149"};
    }
    return {"?", "#8b949e"};
}

} // namespace

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

void RomManagerWindow::build_outbox_tab() {
    m_label_outbox.set_text(_("Outbox:"));
    m_label_outbox.set_halign(Gtk::ALIGN_START);
    m_entry_outbox.set_hexpand(true);
    m_entry_outbox.set_placeholder_text(_("Folder where verified sets are written"));
    m_btn_browse_outbox.set_label(_("Browse..."));
    m_btn_browse_outbox.signal_clicked().connect([this] {
        on_browse(&m_entry_outbox);
        refresh_outbox_view();
    });
    m_outbox_path_grid.set_column_spacing(8);
    m_outbox_path_grid.attach(m_label_outbox,      0, 0, 1, 1);
    m_outbox_path_grid.attach(m_entry_outbox,      1, 0, 1, 1);
    m_outbox_path_grid.attach(m_btn_browse_outbox, 2, 0, 1, 1);

    m_btn_open_outbox.set_label(_("Open folder"));
    m_btn_move_to_library.set_label(_("Move to library"));
    m_btn_refresh_outbox.set_label(_("Refresh"));

    m_outbox_model = Gtk::TreeStore::create(m_outbox_cols);
    m_outbox_view.set_model(m_outbox_model);

    auto* outbox_toggle = Gtk::make_managed<Gtk::CellRendererToggle>();
    outbox_toggle->set_activatable(true);
    outbox_toggle->signal_toggled().connect(sigc::mem_fun(*this, &RomManagerWindow::on_outbox_row_toggled));
    int ocol = m_outbox_view.append_column(_("Move"), *outbox_toggle) - 1;
    if (auto* c = m_outbox_view.get_column(ocol)) {
        c->add_attribute(outbox_toggle->property_active(),      m_outbox_cols.include);
        // System rows are just headers here : only the individual zip they
        // group is a real "move this or not" decision.
        c->add_attribute(outbox_toggle->property_activatable(), m_outbox_cols.is_zip);
        c->add_attribute(outbox_toggle->property_sensitive(),   m_outbox_cols.is_zip);
        c->add_attribute(outbox_toggle->property_visible(),     m_outbox_cols.is_zip);
    }
    m_outbox_view.append_column(_("System / Set"), m_outbox_cols.name);
    m_outbox_view.append_column(_("Sets"),         m_outbox_cols.count);
    m_outbox_view.append_column(_("Size"),         m_outbox_cols.size);
    for (auto* c : m_outbox_view.get_columns()) c->set_resizable(true);

    m_outbox_scroll.add(m_outbox_view);
    m_outbox_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    m_outbox_summary.set_halign(Gtk::ALIGN_START);
    m_outbox_summary.set_text(_("Outbox is empty."));

    m_btn_refresh_outbox.signal_clicked().connect(
        sigc::mem_fun(*this, &RomManagerWindow::refresh_outbox_view));
    m_btn_open_outbox.signal_clicked().connect(
        sigc::mem_fun(*this, &RomManagerWindow::on_open_outbox_clicked));
    m_btn_move_to_library.signal_clicked().connect(
        sigc::mem_fun(*this, &RomManagerWindow::on_move_to_library_clicked));
    m_btn_move_to_library.get_style_context()->add_class("accent-button");

    m_outbox_buttons.set_layout(Gtk::BUTTONBOX_END);
    m_outbox_buttons.set_spacing(6);
    m_outbox_buttons.pack_start(m_btn_refresh_outbox);
    m_outbox_buttons.pack_start(m_btn_open_outbox);
    m_outbox_buttons.pack_start(m_btn_move_to_library);

    m_outbox_box.set_margin_start(10);
    m_outbox_box.set_margin_end(10);
    m_outbox_box.set_margin_top(10);
    m_outbox_box.set_margin_bottom(10);
    m_outbox_box.pack_start(m_outbox_path_grid, Gtk::PACK_SHRINK);
    m_outbox_box.pack_start(m_outbox_summary, Gtk::PACK_SHRINK);
    m_outbox_box.pack_start(m_outbox_scroll,  Gtk::PACK_EXPAND_WIDGET);
    m_outbox_box.pack_start(m_outbox_buttons, Gtk::PACK_SHRINK);
}

void RomManagerWindow::refresh_outbox_view() {
    m_outbox_model->clear();
    const std::string outbox = m_entry_outbox.get_text();
    std::error_code ec;
    if (outbox.empty() || !fs::is_directory(outbox, ec)) {
        m_outbox_summary.set_text(_("Outbox is empty."));
        return;
    }

    int total_sets = 0;
    uintmax_t total_bytes = 0;

    std::vector<fs::path> systems;
    for (auto it = fs::directory_iterator(outbox, ec); it != fs::directory_iterator(); ++it)
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

        auto parent = *(m_outbox_model->append());
        parent[m_outbox_cols.name]  = sysdir.filename().string();
        parent[m_outbox_cols.count] = std::to_string(zips.size());
        parent[m_outbox_cols.size]  = human_size(sys_bytes);

        for (const auto& z : zips) {
            auto child = *(m_outbox_model->append(parent.children()));
            child[m_outbox_cols.is_zip]    = true;
            child[m_outbox_cols.include]   = true; // pre-selected, like every other tab
            child[m_outbox_cols.name]      = z.filename().string();
            child[m_outbox_cols.size]      = human_size(fs::file_size(z, ec));
            child[m_outbox_cols.full_path] = z.string();
        }

        total_sets += (int)zips.size();
        total_bytes += sys_bytes;
    }

    m_outbox_summary.set_text(total_sets == 0
        ? Glib::ustring(_("Outbox is empty."))
        : Glib::ustring::compose(_("%1 set(s) across %2 system(s) : %3"),
                                 total_sets, (int)m_outbox_model->children().size(),
                                 human_size(total_bytes)));
}

void RomManagerWindow::on_outbox_row_toggled(const Glib::ustring& path) {
    auto it = m_outbox_model->get_iter(path);
    if (!it) return;
    if (!(*it)[m_outbox_cols.is_zip]) return;
    (*it)[m_outbox_cols.include] = !(bool)(*it)[m_outbox_cols.include];
}

void RomManagerWindow::on_open_outbox_clicked() {
    const std::string outbox = m_entry_outbox.get_text();
    std::error_code ec;
    if (outbox.empty() || !fs::is_directory(outbox, ec)) return;
    try {
        Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(outbox));
    } catch (const Glib::Error& e) {
        Gtk::MessageDialog dlg(*this, _("Could not open the folder."), false,
                               Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        dlg.set_secondary_text(e.what());
        dlg.run();
    }
}

void RomManagerWindow::on_move_to_library_clicked() {
    const std::string outbox = m_entry_outbox.get_text();
    std::error_code ec;
    if (outbox.empty() || !fs::is_directory(outbox, ec)) return;

    // The outbox mirrors the DAT layout one folder per system, and those folder
    // names match the basenames of the user's configured ROM directories : so a
    // system folder maps onto an existing ROM directory by name, no guessing.
    std::unordered_map<std::string, std::string> by_name;
    for (const auto& p : read_roms_paths())
        by_name[fs::path(p).filename().string()] = p;

    // Only checked zips, read straight from the tree so this matches exactly
    // what the user sees (and unchecked) in the Outbox view.
    struct Candidate { fs::path zip; std::string system_dir; std::string system; };
    std::vector<Candidate> movable, unmapped;
    for (const auto& sysrow : m_outbox_model->children()) {
        std::string sysname = Glib::ustring(sysrow[m_outbox_cols.name]).raw();
        // "FinalBurn Neo - GBA Games" -> "GBA" : the same fallback formula
        // RomAudit uses in the other direction (system -> dat_header), so a
        // folder built from that formula always parses back to the exact
        // system string the DAT uses.
        std::string system = sysname;
        const std::string prefix = "FinalBurn Neo - ", suffix = " Games";
        if (system.rfind(prefix, 0) == 0) system = system.substr(prefix.size());
        if (system.size() > suffix.size() &&
            system.compare(system.size() - suffix.size(), suffix.size(), suffix) == 0)
            system = system.substr(0, system.size() - suffix.size());

        auto match = by_name.find(sysname);
        for (const auto& row : sysrow.children()) {
            if (!row[m_outbox_cols.is_zip] || !row[m_outbox_cols.include]) continue;
            fs::path zip(Glib::ustring(row[m_outbox_cols.full_path]).raw());
            if (match != by_name.end()) movable.push_back({zip, match->second, system});
            else                        unmapped.push_back({zip, sysname, system});
        }
    }
    if (movable.empty() && unmapped.empty()) return;

    // The DAT is the ground truth for what "correct" means : not whatever
    // happens to already sit in the library. Before any set is allowed anywhere
    // near an overwrite, every non-nodump ROM the DAT requires for it must be
    // physically present in the *incoming* archive with the right CRC/size.
    // This is what "repair" has to mean: a full rebuild that satisfies the DAT,
    // not "has at least as many files as before" : that comparison is exactly
    // what let a BIOS-only rebuild look like progress instead of the disaster
    // it was.
    auto verify_against_dat = [&](const fs::path& zip, const std::string& system,
                                   std::string& reason) -> bool {
        std::string game_name = zip.stem().string();
        Game game = m_db->getGame(game_name, system);
        if (game.roms.empty()) {
            reason = "not found in the DAT for system \"" + system + "\"";
            return false;
        }
        std::vector<RomArchive::Entry> entries;
        if (!RomArchive::read_entries(zip.string(), entries)) {
            reason = "cannot read the archive";
            return false;
        }
        std::unordered_map<std::string, const RomArchive::Entry*> by_entry_name;
        for (const auto& e : entries) by_entry_name[e.name] = &e;

        for (const auto& rom : game.roms) {
            if (rom.crc.empty()) continue; // nodump: the DAT itself has no data to check against
            auto it = by_entry_name.find(rom.name);
            if (it == by_entry_name.end())
                it = by_entry_name.find(RomScanner::normalize_name(rom.name));
            if (it == by_entry_name.end()) {
                reason = "missing ROM required by the DAT: " + rom.name;
                return false;
            }
            unsigned long want_crc = strtoul(rom.crc.c_str(), nullptr, 16);
            if (it->second->crc != want_crc) {
                reason = "CRC mismatch for " + rom.name;
                return false;
            }
            if (rom.size && it->second->size != (uint64_t)rom.size) {
                reason = "size mismatch for " + rom.name;
                return false;
            }
        }
        return true;
    };

    Glib::ustring msg = Glib::ustring::compose(
        _("%1 selected set(s) will be moved from the outbox into your existing ROM "
          "directories, matched by system folder name.\n\nA ROM scan will run "
          "automatically afterwards to promote them to available."), (int)movable.size());
    if (!unmapped.empty())
        msg += Glib::ustring::compose(
            _("\n\n%1 selected set(s) sit in a system folder that matches none of your "
              "configured ROM directories and will stay in the outbox."), (int)unmapped.size());

    ConfirmationDialog confirm(*this, _("Move selected sets into the library?"), msg, "📦");
    if (!confirm.show_and_confirm()) return;

    push_log("[MOVE-TO-LIBRARY] starting: " + std::to_string(movable.size()) + " movable, " +
             std::to_string(unmapped.size()) + " unmapped");

    int moved = 0, failed = 0, refused = 0;
    std::set<fs::path> touched_dirs;
    RomManifest::Manifest outbox_manifest = RomManifest::Manifest::load(outbox);
    for (const auto& cand : movable) {
        fs::path dest = fs::path(cand.system_dir) / cand.zip.filename();
        touched_dirs.insert(cand.zip.parent_path());
        bool dest_existed_before = fs::exists(dest, ec);

        // The DAT decides, not a comparison against whatever's already there:
        // refuse outright unless every ROM the DAT requires for this set is
        // physically present with the right CRC/size in the incoming archive.
        std::string reason;
        if (!verify_against_dat(cand.zip, cand.system, reason)) {
            push_log("[MOVE-TO-LIBRARY] REFUSED " + cand.zip.filename().string() +
                     " -> " + dest.string() + " (" + reason + ")");
            ++refused;
            continue;
        }

        // Overwrite on purpose: the whole point of the outbox is a verified
        // replacement for whatever's currently in the library : most often a
        // set Import just rebuilt because the existing one was wrong. rename()
        // replaces the destination atomically on its own; copy_file() (the
        // cross-device fallback) needs to be told to as well, or it fails
        // outright when the target already exists.
        std::error_code mec;
        fs::rename(cand.zip, dest, mec);
        std::string how = "rename";
        if (mec) {
            std::error_code rename_err = mec;
            fs::copy_file(cand.zip, dest, fs::copy_options::overwrite_existing, mec);
            if (!mec) fs::remove(cand.zip, mec);
            how = mec ? ("copy_file failed: " + mec.message() + " (rename had failed: " + rename_err.message() + ")")
                      : "copy_file+remove (rename failed: " + rename_err.message() + ")";
        }
        mec ? failed++ : moved++;
        if (!mec) outbox_manifest.remove(outbox_manifest.relative(cand.zip.string()),
                                         RomManifest::outcome::MovedToLibrary);
        push_log("[MOVE-TO-LIBRARY] " + std::string(mec ? "FAILED " : "ok ") +
                 cand.zip.filename().string() + " -> " + dest.string() +
                 " (" + how + (dest_existed_before ? ", overwrote existing" : "") + ")");
    }
    if (moved > 0) outbox_manifest.save();

    // A system folder emptied by the move above shouldn't linger : that's the
    // whole point of an outbox: once its contents are in the library, it goes
    // back to being empty, not a graveyard of leftover folders.
    for (const auto& dir : touched_dirs) {
        std::error_code rmec;
        if (fs::is_empty(dir, rmec) && !rmec) fs::remove(dir, rmec);
    }

    push_log("[MOVE-TO-LIBRARY] done: " + std::to_string(moved) + " moved, " +
             std::to_string(failed) + " failed, " + std::to_string(refused) + " refused (would shrink)");

    refresh_outbox_view();
    if (refused > 0)
        m_outbox_summary.set_text(Glib::ustring::compose(
            _("Moved %1 set(s), %2 could not be moved, %3 refused because they don't satisfy every ROM the DAT requires : check the log."),
            moved, failed, refused));
    else
        m_outbox_summary.set_text(failed == 0
            ? Glib::ustring::compose(_("Moved %1 set(s) into the library."), moved)
            : Glib::ustring::compose(_("Moved %1 set(s) into the library, %2 could not be moved."),
                                     moved, failed));

    if (moved > 0) m_sig_scan_requested.emit();
}

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
        if (rm.contains("inbox_path")  && rm["inbox_path"].is_string())
            m_entry_inbox.set_text(rm["inbox_path"].get<std::string>());
        if (rm.contains("outbox_path") && rm["outbox_path"].is_string())
            m_entry_outbox.set_text(rm["outbox_path"].get<std::string>());
        if (rm.contains("inbox_recursive") && rm["inbox_recursive"].is_boolean())
            m_check_recursive.set_active(rm["inbox_recursive"].get<bool>());
        if (rm.contains("quarantine_path") && rm["quarantine_path"].is_string())
            m_entry_quarantine.set_text(rm["quarantine_path"].get<std::string>());
    }

    refresh_outbox_view();
    refresh_quarantine_view();
    refresh_dat_list();
}

void RomManagerWindow::refresh_after_scan() {
    refresh_outbox_view();
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
    j["rom_manager"]["inbox_path"]      = m_entry_inbox.get_text().raw();
    j["rom_manager"]["outbox_path"]     = m_entry_outbox.get_text().raw();
    j["rom_manager"]["inbox_recursive"] = m_check_recursive.get_active();
    j["rom_manager"]["quarantine_path"] = m_entry_quarantine.get_text().raw();
    j["dat_path"]                       = m_entry_dat.get_text().raw();

    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}
