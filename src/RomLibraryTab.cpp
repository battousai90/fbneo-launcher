// src/RomLibraryTab.cpp
#include "RomLibraryTab.h"

#include "ConfirmationDialog.h"
#include "RomCleanup.h"
#include "RomManifest.h"
#include "i18n.h"

#include <giomm/appinfo.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace ui = SettingsUi;

namespace {

constexpr int UI_DISPATCH_INTERVAL_MS = 100;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string crc_hex(unsigned long crc) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08lx", crc);
    return buf;
}

std::string human_size(uintmax_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10.0 && u > 0 ? 1 : 0) << v << ' ' << units[u];
    return os.str();
}

std::string join(const std::vector<std::string>& items, const char* sep, size_t max_items = 0) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (max_items && i == max_items) { out += sep; out += "… (+" + std::to_string(items.size() - max_items) + ")"; break; }
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

std::string format_time(int64_t t) {
    if (t <= 0) return "";
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

const char* state_label(RomAudit::RomState s) {
    switch (s) {
        case RomAudit::RomState::Present:   return N_("Present");
        case RomAudit::RomState::WrongName: return N_("Wrong name");
        case RomAudit::RomState::Corrupt:   return N_("Corrupt");
        default:                            return N_("Absent");
    }
}

// XML text escaping for the DAT export.
std::string xml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string csv(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}

} // namespace

// ═══ Construction ═══════════════════════════════════════════════════════════

RomLibraryTab::RomLibraryTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_db(std::move(db)), m_paths(std::move(paths)) {
    get_style_context()->add_class("set-page");

    build_header();
    build_summary();
    build_table();
    build_detail();
    build_footer();

    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomLibraryTab::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &RomLibraryTab::on_worker_finished));

    update_last_audit_label();
    update_summary();
    update_action_buttons();
    show_all_children();
    m_progress.hide();
    m_btn_cancel->hide();
}

RomLibraryTab::~RomLibraryTab() {
    if (m_worker.joinable()) {
        m_cancelled = true;
        m_worker.join();
    }
}

void RomLibraryTab::build_header() {
    // ── Library scan: the DAT group and the two actions, kept separate ────
    auto scan = ui::card("bc-search.svg", _("Library scan"),
                         _("Scan your ROM directories and compare them with DAT files."));
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
    body->set_margin_top(10);

    auto* group_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    auto* group_label = ui::title_label(_("DAT group"));
    group_label->set_valign(Gtk::ALIGN_CENTER);
    group_line->pack_start(*group_label, Gtk::PACK_SHRINK);
    // One group for now : the model behind can hold several, the screen shows
    // the only one an emulator exists for.
    m_dat_group.append(Glib::ustring::compose(_("FinalBurn Neo (%1 DAT files)"), m_db->countDatFiles()));
    m_dat_group.set_active(0);
    m_dat_group.set_size_request(ui::kFieldWidth, -1);
    group_line->pack_start(m_dat_group, Gtk::PACK_SHRINK);
    body->pack_start(*group_line, Gtk::PACK_SHRINK);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_btn_scan = ui::button(_("Scan ROMs"), "bc-sync.svg", ui::Tone::Accent);
    m_btn_scan->set_tooltip_text(_("Rescan every configured ROM directory for new or changed files."));
    m_btn_scan->signal_clicked().connect([this] { if (!m_busy) m_sig_rescan.emit(); });
    m_btn_audit = ui::button(_("Audit library"), "bc-chart.svg");
    m_btn_audit->set_tooltip_text(_("Compare the last scan with the DAT group."));
    m_btn_audit->signal_clicked().connect(sigc::mem_fun(*this, &RomLibraryTab::on_audit_clicked));
    actions->pack_start(*m_btn_scan,  Gtk::PACK_SHRINK);
    actions->pack_start(*m_btn_audit, Gtk::PACK_SHRINK);
    body->pack_start(*actions, Gtk::PACK_SHRINK);
    scan.body->pack_start(*body, Gtk::PACK_SHRINK);
    scan.frame->set_size_request(360, -1);
    m_top.pack_start(*scan.frame, Gtk::PACK_SHRINK);
}

void RomLibraryTab::build_summary() {
    auto sum = ui::card("bc-chart.svg", _("Scan summary"), _("Comparison result for your library."));
    m_last_audit.set_xalign(1.0f);
    m_last_audit.set_justify(Gtk::JUSTIFY_RIGHT);
    m_last_audit.get_style_context()->add_class("set-sub");
    m_last_audit.set_valign(Gtk::ALIGN_CENTER);
    sum.head->pack_end(m_last_audit, Gtk::PACK_SHRINK);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
    body->set_margin_top(10);

    // The counters are the filter: every pill but Total narrows the table.
    m_pill_total     = Gtk::make_managed<ui::Pill>(_("Total"),     ui::PillTone::Neutral);
    m_pill_correct   = Gtk::make_managed<ui::Pill>(_("Correct"),   ui::PillTone::Ok,      true);
    m_pill_missing   = Gtk::make_managed<ui::Pill>(_("Missing"),   ui::PillTone::Error,   true);
    m_pill_incorrect = Gtk::make_managed<ui::Pill>(_("Incorrect"), ui::PillTone::Warn,    true);
    m_pill_fixable   = Gtk::make_managed<ui::Pill>(_("Fixable"),   ui::PillTone::Accent,  true);
    m_pill_orphan    = Gtk::make_managed<ui::Pill>(_("Orphan"),    ui::PillTone::Neutral, true);
    m_pill_ignored   = Gtk::make_managed<ui::Pill>(_("Ignored"),   ui::PillTone::Neutral, true);
    for (auto* p : {m_pill_total, m_pill_correct, m_pill_missing, m_pill_incorrect,
                    m_pill_fixable, m_pill_orphan, m_pill_ignored}) {
        p->set_count(0);
        m_pills.pack_start(*p, Gtk::PACK_SHRINK);
    }
    // Problems first : what the tab is for.
    for (auto* p : {m_pill_missing, m_pill_incorrect, m_pill_fixable, m_pill_orphan}) p->set_active(true);
    for (auto* p : {m_pill_correct, m_pill_missing, m_pill_incorrect, m_pill_fixable, m_pill_orphan, m_pill_ignored})
        p->signal_toggled().connect([this](bool) { refilter(); });
    body->pack_start(m_pills, Gtk::PACK_SHRINK);

    auto* under = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_status.set_xalign(0.0f);
    m_status.get_style_context()->add_class("set-sub");
    m_status.set_ellipsize(Pango::ELLIPSIZE_END);
    under->pack_start(m_status, Gtk::PACK_EXPAND_WIDGET);

    m_btn_export = Gtk::make_managed<Gtk::MenuButton>();
    auto* export_face = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    export_face->pack_start(*ui::image("bc-save.svg", ui::kIconButton), Gtk::PACK_SHRINK);
    export_face->pack_start(*Gtk::make_managed<Gtk::Label>(_("Export…")), Gtk::PACK_SHRINK);
    export_face->pack_start(*ui::image("bc-chevron-down.svg", 14), Gtk::PACK_SHRINK);
    m_btn_export->add(*export_face);
    m_btn_export->set_valign(Gtk::ALIGN_CENTER);
    struct Fmt { const char* label; int id; };
    for (Fmt f : {Fmt{N_("Text report"), 0}, Fmt{N_("CSV (spreadsheet)"), 1}, Fmt{N_("Missing sets as DAT"), 2}}) {
        auto* item = Gtk::make_managed<Gtk::MenuItem>(_(f.label));
        int id = f.id;
        item->signal_activate().connect([this, id] { on_export(id); });
        m_export_menu.append(*item);
    }
    m_export_menu.show_all();
    m_btn_export->set_popup(m_export_menu);
    under->pack_end(*m_btn_export, Gtk::PACK_SHRINK);
    body->pack_start(*under, Gtk::PACK_SHRINK);

    m_bios_line.set_xalign(0.0f);
    m_bios_line.set_line_wrap(true);
    m_bios_line.get_style_context()->add_class("set-warn");
    m_bios_line.set_no_show_all(true);
    body->pack_start(m_bios_line, Gtk::PACK_SHRINK);

    sum.body->pack_start(*body, Gtk::PACK_SHRINK);
    m_top.pack_start(*sum.frame, Gtk::PACK_EXPAND_WIDGET);
    pack_start(m_top, Gtk::PACK_SHRINK);
}

void RomLibraryTab::build_table() {
    m_filter = Gtk::make_managed<ui::FilterBar>(_("Search (game, rom, filename, parent, crc…)"));
    m_system_combo = m_filter->add_combo(_("System:"));
    m_system_combo->append(_("All"));
    m_system_combo->set_active(0);
    m_filter->signal_changed().connect(sigc::mem_fun(*this, &RomLibraryTab::refilter));
    pack_start(*m_filter, Gtk::PACK_SHRINK);

    m_store    = Gtk::ListStore::create(m_cols);
    m_filtered = Gtk::TreeModelFilter::create(m_store);
    m_filtered->set_visible_func(sigc::mem_fun(*this, &RomLibraryTab::row_visible));
    m_sorted   = Gtk::TreeModelSort::create(m_filtered);
    m_sorted->set_sort_column(m_cols.game, Gtk::SORT_ASCENDING);

    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_MULTIPLE);
    m_table->view().set_model(m_sorted);
    m_table->add_check_column(m_cols.include, sigc::mem_fun(*this, &RomLibraryTab::on_row_toggled));
    {
        // Status is painted with the application's state colours, read from
        // the sheet when the tab is on screen (see ensure_colours).
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("Status"), *renderer);
        col->add_attribute(renderer->property_text(), m_cols.status);
        col->set_sort_column(m_cols.status_key);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            const Glib::ustring key = (*it)[m_cols.status_key];
            const bool ignored = (*it)[m_cols.ignored];
            Gdk::RGBA c = m_colours.muted;
            if (ignored)                      c = m_colours.muted;
            else if (key == "available")      c = m_colours.ok;
            else if (key == "missing")        c = m_colours.err;
            else if (key == "incorrect")      c = m_colours.warn;
            else if (key == "orphan")         c = m_colours.accent;
            renderer->property_foreground_rgba() = c;
            renderer->property_weight() = ignored ? Pango::WEIGHT_NORMAL : Pango::WEIGHT_BOLD;
        });
        m_table->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.expand = true; o.min_width = 220; m_table->add_text_column(_("Game / ROM"), m_cols.game, o); }
    m_table->add_text_column(_("System"), m_cols.system);
    m_table->add_text_column(_("Parent"), m_cols.parent);
    { ui::ColumnOptions o; o.mono = true; m_table->add_text_column(_("Expected file"), m_cols.expected, o); }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; m_table->add_text_column(_("Your file"), m_cols.yours, o); }
    { ui::ColumnOptions o; o.expand = true; o.sortable = false; m_table->add_text_column(_("Details"), m_cols.details, o); }
    m_table->view().get_selection()->signal_changed().connect(sigc::mem_fun(*this, &RomLibraryTab::on_selection_changed));
    m_table->signal_context_menu().connect(sigc::mem_fun(*this, &RomLibraryTab::on_context_menu));
    pack_start(*m_table, Gtk::PACK_EXPAND_WIDGET);
}

void RomLibraryTab::build_detail() {
    m_detail = Gtk::make_managed<ui::DetailPanel>("bc-file.svg", _("Selected set"),
                                                  _("Every ROM of the selected set, and where it was found."), 190);
    m_detail->show_placeholder(_("Select a set to see its ROMs."));
    pack_start(*m_detail, Gtk::PACK_SHRINK);
}

void RomLibraryTab::build_footer() {
    m_progress.set_show_text(true);
    m_progress.set_size_request(220, -1);
    m_progress.set_valign(Gtk::ALIGN_CENTER);
    m_progress.set_no_show_all(true);
    m_btn_cancel = ui::button(_("Cancel"), "bc-close.svg");
    m_btn_cancel->set_no_show_all(true);
    m_btn_cancel->signal_clicked().connect([this] { m_cancelled = true; m_status.set_text(_("Cancelling…")); });
    m_footer.pack_start(m_progress, Gtk::PACK_SHRINK);
    m_footer.pack_start(*m_btn_cancel, Gtk::PACK_SHRINK);

    m_btn_select_all  = ui::button(_("Select all"));
    m_btn_select_none = ui::button(_("Select none"));
    m_btn_select_all->signal_clicked().connect([this] { set_all_checked(true); });
    m_btn_select_none->signal_clicked().connect([this] { set_all_checked(false); });
    m_footer.pack_start(*m_btn_select_all,  Gtk::PACK_SHRINK);
    m_footer.pack_start(*m_btn_select_none, Gtk::PACK_SHRINK);

    m_btn_send = ui::button(_("Send fixable to Import"), "bc-download.svg", ui::Tone::Accent);
    m_btn_send->set_tooltip_text(_("Copy the checked fixable sets into the import folder, where Fix rebuilds them from the library."));
    m_btn_send->signal_clicked().connect(sigc::mem_fun(*this, &RomLibraryTab::on_send_to_import_clicked));
    m_btn_quarantine = ui::button(_("Quarantine selected"), "bc-shield.svg");
    m_btn_quarantine->set_tooltip_text(_("Move the checked unrepairable sets to quarantine, extract their extra files, and send orphans to Import for re-identification."));
    m_btn_quarantine->signal_clicked().connect(sigc::mem_fun(*this, &RomLibraryTab::on_quarantine_clicked));
    m_footer.pack_end(*m_btn_send, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_quarantine, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);
}

void RomLibraryTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok     = ui::probe_color(*this, "set-ok");
    m_colours.warn   = ui::probe_color(*this, "set-warn");
    m_colours.err    = ui::probe_color(*this, "set-err");
    m_colours.muted  = ui::probe_color(*this, "set-sub");
    m_colours.accent = ui::probe_color(*this, "set-accent-text");
    m_colours.ready  = true;
}

// ═══ Audit ══════════════════════════════════════════════════════════════════

void RomLibraryTab::on_audit_clicked() {
    if (m_busy) return;
    m_job_paths = m_paths();
    m_cancelled = false;
    m_job = Job::Audit;
    set_busy(true);
    m_status.set_text(_("Auditing…"));
    m_worker = std::thread(&RomLibraryTab::worker_audit, this);
}

void RomLibraryTab::worker_audit() {
    RomInbox::Callbacks cb = make_callbacks();
    // Everything, not only problems : the table filters, and "Correct" is a
    // pill like the others.
    RomAudit::Report rep = RomAudit::audit(m_db, m_job_paths.roms_paths, /*problems_only=*/false, cb);
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_audit = std::move(rep);
    }
    m_finished_dispatcher();
}

void RomLibraryTab::refresh_after_scan() {
    if (m_audit_ever_run && !m_busy) on_audit_clicked();
}

void RomLibraryTab::populate() {
    // Detach the model while filling it : a view bound to a filter re-runs
    // the visible function on every row inserted.
    m_table->view().unset_model();
    m_store->clear();

    std::set<std::string> systems;
    for (size_t i = 0; i < m_audit.games.size(); ++i) {
        const auto& g = m_audit.games[i];
        auto row = *(m_store->append());
        systems.insert(g.system);

        std::string expected = g.name + ".zip";
        std::string yours = g.archive_found ? fs::path(g.archive).filename().string() : "-";

        bool can_quarantine = g.status == "incorrect" && !g.repairable && !g.ignored
                              && g.archive_found && !g.archive.empty();
        bool has_extras = !g.extra_entries.empty() && g.archive_found && !g.archive.empty();

        std::vector<std::string> bits;
        if (g.absent)  bits.push_back(Glib::ustring::compose(_("%1 absent"),   g.absent).raw());
        if (g.corrupt) bits.push_back(Glib::ustring::compose(_("%1 corrupt"),  g.corrupt).raw());
        if (g.wrong)   bits.push_back(Glib::ustring::compose(_("%1 misnamed"), g.wrong).raw());
        if (has_extras) bits.push_back(Glib::ustring::compose(_("%1 extra file(s) not needed by the DAT"), (int)g.extra_entries.size()).raw());
        if (!g.archive_found && g.status != "available") bits.push_back(_("no archive found"));
        else if (g.repairable) bits.push_back(_("repairable from the library"));
        int inherited = 0;
        for (const auto& r : g.roms) if (!r.inherited_from.empty()) ++inherited;
        if (inherited) bits.push_back(Glib::ustring::compose(_("%1 from parent/BIOS"), inherited).raw());
        if (g.ignored) bits.insert(bits.begin(), _("ignored"));
        if (g.is_bios) bits.insert(bits.begin(), _("BIOS"));

        const char* label = g.status == "available" ? N_("Correct")
                          : g.status == "incorrect" ? N_("Incorrect") : N_("Missing");
        row[m_cols.include]    = false;
        row[m_cols.status]     = g.ignored ? Glib::ustring(_("Ignored")) : Glib::ustring(_(label));
        row[m_cols.status_key] = g.status;
        row[m_cols.game]       = g.description.empty() ? g.name : g.name + "  (" + g.description + ")";
        row[m_cols.system]     = g.system;
        row[m_cols.parent]     = g.cloneof;
        row[m_cols.expected]   = expected;
        row[m_cols.yours]      = yours;
        row[m_cols.details]    = join(bits, ", ", 4);
        row[m_cols.kind]       = KIND_SET;
        row[m_cols.index]      = (unsigned int)i;
        row[m_cols.repairable] = g.repairable;
        row[m_cols.ignored]    = g.ignored;
        row[m_cols.has_extras] = has_extras;
        row[m_cols.actionable] = can_quarantine || has_extras || g.repairable;

        std::string blob = lower(g.name + ' ' + g.description + ' ' + expected + ' ' + yours + ' ' + g.cloneof + ' ' + g.system);
        for (const auto& r : g.roms) {
            blob += ' ' + lower(r.name) + ' ' + crc_hex(r.crc);
            if (!r.found_as.empty()) blob += ' ' + lower(r.found_as);
        }
        row[m_cols.search_blob] = blob;
    }

    for (size_t i = 0; i < m_audit.orphans.size(); ++i) {
        const auto& o = m_audit.orphans[i];
        std::string base = fs::path(o.path).filename().string();
        std::string folder = fs::path(o.path).parent_path().filename().string();
        int elsewhere = 0;
        for (const auto& e : o.entries) if (e.copy_elsewhere) ++elsewhere;
        auto row = *(m_store->append());
        row[m_cols.include]    = false;
        row[m_cols.status]     = _("Orphan");
        row[m_cols.status_key] = "orphan";
        row[m_cols.game]       = base;
        row[m_cols.system]     = folder;
        row[m_cols.parent]     = "";
        row[m_cols.expected]   = "";
        row[m_cols.yours]      = base;
        row[m_cols.details]    = Glib::ustring::compose(
            _("Not in DAT : %1 file(s), %2 with a copy elsewhere in the library"), (int)o.entries.size(), elsewhere);
        row[m_cols.kind]       = KIND_ORPHAN;
        row[m_cols.index]      = (unsigned int)i;
        row[m_cols.repairable] = false;
        row[m_cols.ignored]    = false;
        row[m_cols.has_extras] = false;
        row[m_cols.actionable] = true;
        std::string blob = lower(base + ' ' + folder);
        for (const auto& e : o.entries) blob += ' ' + lower(e.name) + ' ' + crc_hex(e.crc);
        row[m_cols.search_blob] = blob;
    }

    // Systems seen this time, current choice kept when still there.
    Glib::ustring chosen = m_system_combo->get_active_text();
    m_system_combo->remove_all();
    m_system_combo->append(_("All"));
    for (const auto& s : systems) m_system_combo->append(s);
    m_system_combo->set_active(0);
    if (!chosen.empty() && chosen != _("All")) {
        int idx = 1;
        for (const auto& s : systems) { if (s == chosen.raw()) { m_system_combo->set_active(idx); break; } ++idx; }
    }

    m_table->view().set_model(m_sorted);
    refilter();
    update_summary();
    update_action_buttons();
    m_detail->show_placeholder(_("Select a set to see its ROMs."));
}

void RomLibraryTab::update_summary() {
    m_pill_total->set_count(m_audit.total);
    m_pill_correct->set_count(m_audit.available);
    m_pill_missing->set_count(m_audit.missing);
    m_pill_incorrect->set_count(m_audit.incorrect);
    m_pill_fixable->set_count(m_audit.repairable);
    m_pill_orphan->set_count((long)m_audit.orphans.size());
    m_pill_ignored->set_count(m_audit.ignored);

    if (!m_audit_ever_run) {
        m_status.set_text(_("Run an audit to compare your library with the DAT group."));
    } else if (m_audit.pool_empty) {
        m_status.set_text(_("The scan cache is empty : run a ROM scan first."));
    } else {
        m_status.set_text(Glib::ustring::compose(
            _("%1 set(s) with a problem, of which %2 can be repaired from the library itself. Collection style: %3."),
            m_audit.incorrect + m_audit.missing, m_audit.repairable, RomResolve::to_string(m_audit.style)));
    }

    if (m_audit.missing_bios.empty()) {
        m_bios_line.hide();
    } else {
        std::vector<std::string> parts;
        for (const auto& b : m_audit.missing_bios)
            parts.push_back(Glib::ustring::compose(_("%1 (%2) : %3 dependent set(s)"), b.name, b.system, b.dependents).raw());
        m_bios_line.set_text(Glib::ustring::compose(_("BIOS not available : %1. In a split collection those sets cannot run."), join(parts, "; ", 4)));
        m_bios_line.show();
    }
}

void RomLibraryTab::update_last_audit_label() {
    int64_t t = m_db->getScanMetadata("last_audit_time", 0);
    if (t <= 0) {
        m_last_audit.set_text(_("No audit yet"));
    } else {
        m_last_audit.set_text(Glib::ustring::compose(_("Last audit: %1\nCompared with %2 DAT files"),
                                                     format_time(t), m_db->countDatFiles()));
    }
}

bool RomLibraryTab::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const Gtk::TreeModel::Row row = *it;
    const Glib::ustring key = row[m_cols.status_key];
    const bool ignored = row[m_cols.ignored];
    const bool repairable = row[m_cols.repairable];

    bool wanted = false;
    if (ignored)                wanted = m_pill_ignored->active();
    else if (key == "orphan")   wanted = m_pill_orphan->active();
    else if (key == "available") wanted = m_pill_correct->active();
    else {
        if (key == "missing"   && m_pill_missing->active())   wanted = true;
        if (key == "incorrect" && m_pill_incorrect->active()) wanted = true;
        if (repairable         && m_pill_fixable->active())   wanted = true;
    }
    if (!wanted) return false;

    if (m_system_combo->get_active_row_number() > 0) {
        if (row[m_cols.system] != m_system_combo->get_active_text()) return false;
    }
    std::string needle = lower(m_filter->search_text());
    if (!needle.empty()) {
        const Glib::ustring blob = row[m_cols.search_blob];
        if (blob.raw().find(needle) == std::string::npos) return false;
    }
    return true;
}

void RomLibraryTab::refilter() {
    if (!m_filtered) return;
    m_filtered->refilter();
    int shown = (int)m_filtered->children().size();
    m_filter->set_summary(Glib::ustring::compose(_("%1 result(s) (%2 sets audited)"), shown, m_audit.total));
}

Gtk::TreeModel::Row RomLibraryTab::source_row(const Gtk::TreeModel::Path& sorted_path) const {
    auto child = m_filtered->convert_path_to_child_path(m_sorted->convert_path_to_child_path(sorted_path));
    return *m_store->get_iter(child);
}

// ═══ Detail panel ═══════════════════════════════════════════════════════════

void RomLibraryTab::on_selection_changed() {
    auto rows = m_table->view().get_selection()->get_selected_rows();
    if (rows.empty()) { m_detail->show_placeholder(_("Select a set to see its ROMs.")); return; }
    Gtk::TreeModel::Row row = source_row(rows.front());
    if ((int)row[m_cols.kind] == KIND_ORPHAN) show_orphan_detail(row);
    else                                       show_set_detail(row);
}

namespace {
struct RomCols : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::ustring> state, expected, found_as, crc_expected, crc_found, size, found_in;
    RomCols() { add(state); add(expected); add(found_as); add(crc_expected); add(crc_found); add(size); add(found_in); }
};
}

void RomLibraryTab::show_set_detail(const Gtk::TreeModel::Row& row) {
    const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
    m_detail->set_title(g.description.empty() ? g.name : g.name + "  —  " + g.description);
    std::string sub = g.system;
    if (!g.cloneof.empty()) sub += Glib::ustring::compose(_("  ·  clone of %1"), g.cloneof).raw();
    if (g.archive_found) sub += "  ·  " + g.archive;
    m_detail->set_subtitle(sub);

    static RomCols cols;
    auto store = Gtk::ListStore::create(cols);
    for (const auto& r : g.roms) {
        auto rr = *(store->append());
        rr[cols.state]        = _(state_label(r.state));
        rr[cols.expected]     = r.name;
        rr[cols.found_as]     = r.found_as;
        rr[cols.crc_expected] = crc_hex(r.crc);
        rr[cols.crc_found]    = (r.state == RomAudit::RomState::Absent) ? Glib::ustring("-") : Glib::ustring(crc_hex(r.found_crc));
        rr[cols.size]         = human_size(r.size);
        std::string where;
        if (!r.inherited_from.empty())
            where = Glib::ustring::compose(_("from %1 (%2)"), r.inherited_from, fs::path(r.found_in).filename().string()).raw();
        else if (!r.found_in.empty())
            where = Glib::ustring::compose(_("good copy in %1"), fs::path(r.found_in).filename().string()).raw();
        else if (r.inherited && r.state == RomAudit::RomState::Absent)
            where = _("expected from parent/BIOS");
        rr[cols.found_in] = where;
    }
    for (const auto& x : g.extra_entries) {
        auto rr = *(store->append());
        rr[cols.state]    = _("Extra");
        rr[cols.expected] = "";
        rr[cols.found_as] = x;
        rr[cols.found_in] = _("not needed by the DAT");
    }

    auto* t = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    t->view().set_model(store);
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("State"), *renderer);
        col->add_attribute(renderer->property_text(), cols.state);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            static RomCols c;
            const Glib::ustring s = (*it)[c.state];
            Gdk::RGBA colour = m_colours.muted;
            if (s == _("Present"))         colour = m_colours.ok;
            else if (s == _("Absent"))     colour = m_colours.err;
            else if (s == _("Extra"))      colour = m_colours.muted;
            else                           colour = m_colours.warn;
            renderer->property_foreground_rgba() = colour;
        });
        t->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Expected name"), cols.expected, o); }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Found as"), cols.found_as, o); }
    { ui::ColumnOptions o; o.mono = true; t->add_text_column(_("CRC expected"), cols.crc_expected, o); t->add_text_column(_("CRC found"), cols.crc_found, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; t->add_text_column(_("Size"), cols.size, o); }
    { ui::ColumnOptions o; o.expand = true; t->add_text_column(_("Found in"), cols.found_in, o); }
    m_detail->set_content(t);
}

void RomLibraryTab::show_orphan_detail(const Gtk::TreeModel::Row& row) {
    const auto& o = m_audit.orphans[(unsigned int)row[m_cols.index]];
    m_detail->set_title(fs::path(o.path).filename().string());
    m_detail->set_subtitle(Glib::ustring::compose(_("Not claimed by any set of the DAT group  ·  %1"), o.path));

    static RomCols cols;
    auto store = Gtk::ListStore::create(cols);
    for (const auto& e : o.entries) {
        auto rr = *(store->append());
        rr[cols.state]        = e.copy_elsewhere ? _("Copy elsewhere") : _("Unique");
        rr[cols.expected]     = "";
        rr[cols.found_as]     = e.name;
        rr[cols.crc_expected] = "";
        rr[cols.crc_found]    = crc_hex(e.crc);
        rr[cols.size]         = "";
        rr[cols.found_in]     = e.copy_elsewhere ? _("the same data exists in another archive") : _("no other copy in the library");
    }
    auto* t = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    t->view().set_model(store);
    t->add_text_column(_("State"), cols.state);
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Entry"), cols.found_as, o); }
    { ui::ColumnOptions o; o.mono = true; t->add_text_column(_("CRC"), cols.crc_found, o); }
    { ui::ColumnOptions o; o.expand = true; t->add_text_column(_("Note"), cols.found_in, o); }
    m_detail->set_content(t);
}

// ═══ Checkboxes and actions ═════════════════════════════════════════════════

void RomLibraryTab::on_row_toggled(const Glib::ustring& path) {
    Gtk::TreeModel::Row row = source_row(Gtk::TreeModel::Path(path));
    if (!row[m_cols.actionable]) return;
    row[m_cols.include] = !row[m_cols.include];
    update_action_buttons();
}

void RomLibraryTab::set_all_checked(bool on) {
    // Only what is visible : "select all" on a filtered table means the rows
    // one is looking at.
    for (const auto& frow : m_filtered->children()) {
        Gtk::TreeModel::Row row = *m_filtered->convert_iter_to_child_iter(frow);
        if (row[m_cols.actionable]) row[m_cols.include] = on;
    }
    update_action_buttons();
}

std::vector<Gtk::TreeModel::Row> RomLibraryTab::checked_rows() const {
    std::vector<Gtk::TreeModel::Row> out;
    for (const auto& row : m_store->children())
        if (row[m_cols.include]) out.push_back(row);
    return out;
}

void RomLibraryTab::update_action_buttons() {
    int fixable = 0, quarantinable = 0;
    for (const auto& row : checked_rows()) {
        const auto& g_kind = (int)row[m_cols.kind];
        if (g_kind == KIND_ORPHAN) { ++quarantinable; continue; }
        if (row[m_cols.repairable] && !row[m_cols.ignored]) ++fixable;
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        bool can_quarantine = g.status == "incorrect" && !g.repairable && !g.ignored && g.archive_found;
        if (can_quarantine || row[m_cols.has_extras]) ++quarantinable;
    }
    m_btn_send->set_label(fixable ? Glib::ustring::compose(_("Send fixable to Import (%1)"), fixable) : Glib::ustring(_("Send fixable to Import")));
    m_btn_send->set_sensitive(!m_busy && fixable > 0);
    m_btn_quarantine->set_label(quarantinable ? Glib::ustring::compose(_("Quarantine selected (%1)"), quarantinable) : Glib::ustring(_("Quarantine selected")));
    m_btn_quarantine->set_sensitive(!m_busy && quarantinable > 0);
}

void RomLibraryTab::copy_to_clipboard(const Glib::ustring& text, const Glib::ustring& what) {
    if (text.empty()) { flash(Glib::ustring::compose(_("Nothing to copy for %1."), what)); return; }
    Gtk::Clipboard::get()->set_text(text);
    flash(Glib::ustring::compose(_("Copied %1 to the clipboard."), what));
}

std::string RomLibraryTab::all_details_of(const Gtk::TreeModel::Row& row) const {
    std::ostringstream out;
    if ((int)row[m_cols.kind] == KIND_ORPHAN) {
        const auto& o = m_audit.orphans[(unsigned int)row[m_cols.index]];
        out << "Orphan archive: " << o.path << "\n";
        for (const auto& e : o.entries)
            out << "  " << e.name << "\tcrc=" << crc_hex(e.crc) << (e.copy_elsewhere ? "\t(copy elsewhere)" : "") << "\n";
        return out.str();
    }
    const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
    out << g.name << " (" << g.description << ")\n"
        << "system: " << g.system << "\n"
        << "status: " << g.status << (g.ignored ? " (ignored)" : "") << "\n";
    if (!g.cloneof.empty()) out << "parent: " << g.cloneof << "\n";
    out << "expected: " << g.name << ".zip\n"
        << "your file: " << (g.archive_found ? g.archive : "-") << "\n";
    for (const auto& r : g.roms) {
        out << "  " << _(state_label(r.state)) << "\t" << r.name << "\tcrc=" << crc_hex(r.crc)
            << "\tsize=" << r.size;
        if (!r.found_as.empty()) out << "\tfound as " << r.found_as;
        if (r.state == RomAudit::RomState::Corrupt) out << "\tfound crc=" << crc_hex(r.found_crc);
        if (!r.inherited_from.empty()) out << "\tfrom " << r.inherited_from;
        else if (!r.found_in.empty()) out << "\tcopy in " << r.found_in;
        out << "\n";
    }
    for (const auto& x : g.extra_entries) out << "  extra\t" << x << "\n";
    return out.str();
}

void RomLibraryTab::search_on_web(const Gtk::TreeModel::Row& row) {
    Glib::ustring q = (int)row[m_cols.kind] == KIND_ORPHAN
        ? Glib::ustring(row[m_cols.yours])
        : Glib::ustring(row[m_cols.expected]) + " " + Glib::ustring(row[m_cols.system]);
    std::string uri = "https://duckduckgo.com/?q=" + Glib::uri_escape_string(q.raw());
    try { Gio::AppInfo::launch_default_for_uri(uri); }
    catch (const Glib::Error& e) { flash(Glib::ustring::compose(_("Could not open a browser: %1"), e.what())); }
}

void RomLibraryTab::toggle_ignore(const Gtk::TreeModel::Row& row) {
    if ((int)row[m_cols.kind] != KIND_SET) return;
    auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
    bool now_ignored = !g.ignored;
    bool ok = now_ignored ? m_db->ignoreSet(g.name, g.system) : m_db->unignoreSet(g.name, g.system);
    if (!ok) { flash(_("Could not update the ignore list.")); return; }
    g.ignored = now_ignored;

    // Counters move with the set : an ignored set is in neither bucket of
    // problems, and never repairable.
    auto bucket = [&](int delta) {
        if (g.status == "available")      m_audit.available += delta;
        else if (g.status == "incorrect") m_audit.incorrect += delta;
        else                              m_audit.missing   += delta;
        if (g.repairable)                 m_audit.repairable += delta;
    };
    if (now_ignored) { bucket(-1); m_audit.ignored++; }
    else             { bucket(+1); m_audit.ignored--; }

    const char* label = g.status == "available" ? N_("Correct")
                      : g.status == "incorrect" ? N_("Incorrect") : N_("Missing");
    row[m_cols.ignored] = now_ignored;
    row[m_cols.status]  = now_ignored ? Glib::ustring(_("Ignored")) : Glib::ustring(_(label));
    row[m_cols.include] = false;
    row[m_cols.actionable] = !now_ignored && ((g.status == "incorrect" && !g.repairable && g.archive_found)
                                              || row[m_cols.has_extras] || g.repairable);
    Glib::ustring details = row[m_cols.details];
    const Glib::ustring tag = Glib::ustring(_("ignored")) + ", ";
    if (now_ignored) row[m_cols.details] = tag + details;
    else if (details.find(tag) == 0) row[m_cols.details] = details.substr(tag.size());
    update_summary();
    refilter();
    update_action_buttons();
    flash(now_ignored ? Glib::ustring::compose(_("%1 will not be reported as a problem any more."), g.name)
                      : Glib::ustring::compose(_("%1 is reported again."), g.name));
}

void RomLibraryTab::on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*, GdkEventButton* event) {
    Gtk::TreeModel::Row row = source_row(path);
    const bool is_set = (int)row[m_cols.kind] == KIND_SET;

    // Rebuilt each time: the entries depend on the row.
    for (auto* child : m_context_menu.get_children()) m_context_menu.remove(*child);
    auto add = [&](const Glib::ustring& label, std::function<void()> fn, bool enabled = true) {
        auto* item = Gtk::make_managed<Gtk::MenuItem>(label);
        item->set_sensitive(enabled);
        item->signal_activate().connect([fn] { fn(); });
        m_context_menu.append(*item);
    };
    auto sep = [&] { m_context_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>()); };

    const Glib::ustring name     = is_set ? Glib::ustring(m_audit.games[(unsigned int)row[m_cols.index]].name) : Glib::ustring(row[m_cols.yours]);
    const Glib::ustring expected = row[m_cols.expected];
    const Glib::ustring yours    = row[m_cols.yours];
    const Glib::ustring parent   = row[m_cols.parent];

    add(_("Copy game name"),         [this, name]     { copy_to_clipboard(name, _("the game name")); });
    add(_("Copy expected filename"), [this, expected] { copy_to_clipboard(expected, _("the expected filename")); }, !expected.empty());
    add(_("Copy your filename"),     [this, yours]    { copy_to_clipboard(yours, _("your filename")); }, yours != "-" && !yours.empty());
    add(_("Copy parent name"),       [this, parent]   { copy_to_clipboard(parent, _("the parent name")); }, !parent.empty());
    if (is_set) {
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        std::vector<std::string> missing;
        for (const auto& r : g.roms) if (r.state == RomAudit::RomState::Absent) missing.push_back(r.name);
        add(_("Copy missing filenames"), [this, missing] { copy_to_clipboard(join(missing, "\n"), _("the missing filenames")); }, !missing.empty());
    }
    add(_("Copy all details"), [this, row] { copy_to_clipboard(all_details_of(row), _("all details")); });
    sep();
    add(_("Search on web"), [this, row] { search_on_web(row); });
    if (is_set) {
        sep();
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        add(g.ignored ? _("Stop ignoring this set") : _("Ignore this set (do not report again)"),
            [this, row] { toggle_ignore(row); });
        if (g.repairable && !g.ignored)
            add(_("Send to Import"), [this, row] {
                row[m_cols.include] = true; update_action_buttons(); on_send_to_import_clicked(); });
        bool can_quarantine = g.status == "incorrect" && !g.repairable && !g.ignored && g.archive_found;
        if (can_quarantine || row[m_cols.has_extras])
            add(_("Quarantine"), [this, row] {
                row[m_cols.include] = true; update_action_buttons(); on_quarantine_clicked(); });
    } else {
        sep();
        add(_("Send to Import for re-identification"), [this, row] {
            row[m_cols.include] = true; update_action_buttons(); on_quarantine_clicked(); });
    }
    m_context_menu.show_all();
    m_context_menu.popup_at_pointer((GdkEvent*)event);
}

void RomLibraryTab::on_send_to_import_clicked() {
    if (m_busy) return;
    std::vector<std::string> archives;
    for (const auto& row : checked_rows()) {
        if ((int)row[m_cols.kind] != KIND_SET || !row[m_cols.repairable] || row[m_cols.ignored]) continue;
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        if (g.archive_found && !g.archive.empty()) archives.push_back(g.archive);
    }
    if (archives.empty()) { flash(_("Check at least one fixable set first.")); return; }
    m_sig_send_to_import.emit(archives);
}

void RomLibraryTab::on_quarantine_clicked() {
    if (m_busy) return;
    m_qjob = QuarantineJob{};
    for (const auto& row : checked_rows()) {
        if ((int)row[m_cols.kind] == KIND_ORPHAN) {
            const auto& o = m_audit.orphans[(unsigned int)row[m_cols.index]];
            m_qjob.orphans.push_back({o.path, fs::path(o.path).parent_path().filename().string(), Glib::ustring(row[m_cols.system]).raw()});
            continue;
        }
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        if (!g.archive_found || g.archive.empty() || g.ignored) continue;
        std::string header = g.dat_header.empty() ? g.system : g.dat_header;
        if (g.status == "incorrect" && !g.repairable)
            m_qjob.whole.push_back({g.archive, header, g.system});
        else if (!g.extra_entries.empty())
            m_qjob.extras.push_back({g.archive, g.system, header, g.extra_entries});
    }
    if (m_qjob.whole.empty() && m_qjob.orphans.empty() && m_qjob.extras.empty()) {
        flash(_("Check at least one unrepairable set, orphan or archive with extra files first."));
        return;
    }

    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Paths p = m_paths();
    std::error_code ec;
    bool needs_quarantine = !m_qjob.whole.empty() || !m_qjob.extras.empty();
    if (needs_quarantine) {
        if (p.quarantine.empty()) { if (top) ui::notice(*top, _("No quarantine folder"), _("Set a quarantine directory in Settings first.")); return; }
        fs::create_directories(p.quarantine, ec);
        if (ec || !fs::is_directory(p.quarantine, ec)) { if (top) ui::notice(*top, _("Quarantine folder"), _("Could not create the quarantine folder.")); return; }
    }
    if (!m_qjob.orphans.empty()) {
        if (p.inbox.empty()) { if (top) ui::notice(*top, _("No import folder"), _("Set an import folder (Import tab) first.")); return; }
        fs::create_directories(p.inbox, ec);
        if (ec || !fs::is_directory(p.inbox, ec)) { if (top) ui::notice(*top, _("Import folder"), _("Could not create the import folder.")); return; }
    }

    Glib::ustring summary;
    if (!m_qjob.whole.empty())
        summary += Glib::ustring::compose(_("%1 unrepairable set(s) (wrong data, no good copy elsewhere) → quarantine\n"), (int)m_qjob.whole.size());
    if (!m_qjob.extras.empty())
        summary += Glib::ustring::compose(_("%1 archive(s) : extra files not needed by the DAT extracted → quarantine\n"), (int)m_qjob.extras.size());
    if (!m_qjob.orphans.empty())
        summary += Glib::ustring::compose(_("%1 orphan archive(s) matching no DAT entry → import folder, for re-identification\n"), (int)m_qjob.orphans.size());
    summary += _("\nNothing is deleted : every file is moved, never destroyed.");
    if (top) {
        ConfirmationDialog confirm(*top, _("Quarantine selected items?"), summary, "🛡");
        if (!confirm.show_and_confirm()) return;
    }

    m_job_paths = p;
    m_cancelled = false;
    m_job = Job::Quarantine;
    set_busy(true);
    m_status.set_text(_("Moving files…"));
    m_worker = std::thread(&RomLibraryTab::worker_quarantine, this);
}

void RomLibraryTab::worker_quarantine() {
    RomInbox::Callbacks cb = make_callbacks();
    std::error_code ec;
    const std::string& quarantine = m_job_paths.quarantine;
    const std::string& inbox = m_job_paths.inbox;
    RomManifest::Manifest manifest = quarantine.empty() ? RomManifest::Manifest() : RomManifest::Manifest::load(quarantine);

    auto move_file = [&](const fs::path& src, const fs::path& dest) -> bool {
        std::error_code mec;
        fs::rename(src, dest, mec);
        if (!mec) return true;
        fs::copy_file(src, dest, mec);
        if (mec) return false;
        fs::remove(src, mec);
        return true;
    };

    const size_t total = m_qjob.whole.size() + m_qjob.extras.size() + m_qjob.orphans.size();
    size_t done = 0;
    auto step = [&](const std::string& what) { push_progress(100.0 * (double)(++done) / (double)total, what); };

    for (const auto& w : m_qjob.whole) {
        if (m_cancelled) break;
        fs::path src(w.archive);
        fs::path dest_dir = fs::path(quarantine) / w.dat_header;
        fs::create_directories(dest_dir, ec);
        fs::path dest = dest_dir / src.filename();
        step(src.filename().string());
        if (fs::exists(dest, ec)) { m_qjob.failed++; push_log("[QUARANTINE] already there, left alone: " + dest.string()); continue; }
        if (!move_file(src, dest)) { m_qjob.failed++; push_log("[QUARANTINE] FAILED " + src.string()); continue; }
        m_qjob.moved++;
        RomManifest::Entry e;
        e.file = manifest.relative(dest.string());
        e.game = src.stem().string();
        e.system = w.system;
        e.dat_header = w.dat_header;
        e.reason = RomManifest::reason::BadCrc;
        e.origin = w.archive;
        e.action = RomManifest::action::Moved;
        e.details.push_back("wrong data, and no good copy anywhere else in the library");
        manifest.add(std::move(e));
        push_log("[QUARANTINE] " + src.filename().string() + " -> " + dest.string());
    }

    for (const auto& x : m_qjob.extras) {
        if (m_cancelled) break;
        step(fs::path(x.archive).filename().string());
        std::vector<std::string> written;
        bool ok = RomCleanup::extract_entries_to_quarantine(x.archive, x.entries, quarantine, cb, &written);
        ok ? m_qjob.cleaned++ : m_qjob.failed++;
        for (const auto& f : written) {
            RomManifest::Entry e;
            e.file = manifest.relative(f);
            e.game = fs::path(x.archive).stem().string();
            e.system = x.system;
            e.dat_header = x.dat_header;
            e.reason = RomManifest::reason::ExtraFiles;
            e.origin = x.archive;
            e.action = RomManifest::action::Extracted;
            e.details.push_back("entry no DAT rom of this set needs, taken out of an otherwise sound archive");
            manifest.add(std::move(e));
        }
    }

    for (const auto& o : m_qjob.orphans) {
        if (m_cancelled) break;
        fs::path src(o.archive);
        fs::path dest = fs::path(inbox) / src.filename();
        step(src.filename().string());
        if (fs::exists(dest, ec)) { m_qjob.failed++; push_log("[QUARANTINE] inbox already has " + dest.string()); continue; }
        if (!move_file(src, dest)) { m_qjob.failed++; push_log("[QUARANTINE] FAILED " + src.string()); continue; }
        m_qjob.sent++;
        push_log("[QUARANTINE] orphan " + src.filename().string() + " -> inbox");
    }

    if (!quarantine.empty() && (m_qjob.moved > 0 || m_qjob.cleaned > 0) && !manifest.save())
        push_log("[QUARANTINE] could not write " + std::string(RomManifest::kFileName));
    m_finished_dispatcher();
}

// ═══ Export ═════════════════════════════════════════════════════════════════

void RomLibraryTab::on_export(int format) {
    if (!m_audit_ever_run) { flash(_("Run an audit first.")); return; }
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Export audit"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Save"),   Gtk::RESPONSE_OK);
    dlg.set_current_name(format == 0 ? "library-audit.txt" : format == 1 ? "library-audit.csv" : "missing-sets.dat");
    dlg.set_do_overwrite_confirmation(true);
    if (dlg.run() != Gtk::RESPONSE_OK) return;

    std::ofstream out(dlg.get_filename());
    if (!out) { if (top) ui::notice(*top, _("Could not write the file."), dlg.get_filename()); return; }

    if (format == 0) {
        out << "# ROM library audit : Bootcade\n";
        out << "# " << m_audit.total << " sets: " << m_audit.available << " correct, "
            << m_audit.incorrect << " incorrect, " << m_audit.missing << " missing, "
            << m_audit.repairable << " repairable from the library, "
            << m_audit.ignored << " ignored; style " << RomResolve::to_string(m_audit.style) << "\n";
        for (const char* want : {"incorrect", "missing"}) {
            out << "\n\n=== " << want << " ===\n";
            for (const auto& g : m_audit.games) {
                if (g.status != want || g.ignored) continue;
                out << "\n" << g.name << "  (" << g.description << ")  [" << g.system << "]"
                    << (g.repairable ? "  repairable" : "") << "\n";
                if (g.archive_found) out << "    archive: " << g.archive << "\n";
                for (const auto& r : g.roms) {
                    if (r.state == RomAudit::RomState::Present) continue;
                    out << "    " << _(state_label(r.state)) << "\t" << r.name << "\tcrc=" << crc_hex(r.crc) << "\tsize=" << r.size;
                    if (!r.found_as.empty()) out << "\tfound as " << r.found_as;
                    if (!r.found_in.empty()) out << "\tcopy in " << r.found_in;
                    out << "\n";
                }
            }
        }
        if (!m_audit.orphans.empty()) {
            out << "\n\n=== orphan archives ===\n";
            for (const auto& o : m_audit.orphans) out << o.path << "\n";
        }
    } else if (format == 1) {
        out << "status,game,description,system,parent,expected_file,your_file,rom,rom_state,crc_expected,crc_found,size,found_as,found_in,inherited_from\n";
        for (const auto& g : m_audit.games) {
            std::string status = g.ignored ? "ignored" : g.status;
            std::string yours = g.archive_found ? g.archive : "";
            if (g.roms.empty() || g.status == "available") {
                out << csv(status) << ',' << csv(g.name) << ',' << csv(g.description) << ',' << csv(g.system) << ','
                    << csv(g.cloneof) << ',' << csv(g.name + ".zip") << ',' << csv(yours) << ",,,,,,,,\n";
                continue;
            }
            for (const auto& r : g.roms) {
                out << csv(status) << ',' << csv(g.name) << ',' << csv(g.description) << ',' << csv(g.system) << ','
                    << csv(g.cloneof) << ',' << csv(g.name + ".zip") << ',' << csv(yours) << ','
                    << csv(r.name) << ',' << _(state_label(r.state)) << ',' << crc_hex(r.crc) << ','
                    << (r.state == RomAudit::RomState::Absent ? "" : crc_hex(r.found_crc)) << ',' << r.size << ','
                    << csv(r.found_as) << ',' << csv(r.found_in) << ',' << csv(r.inherited_from) << "\n";
            }
        }
        for (const auto& o : m_audit.orphans)
            out << "orphan,,," << csv(fs::path(o.path).parent_path().filename().string()) << ",,," << csv(o.path) << ",,,,,,,,\n";
    } else {
        // A datafile of what is missing : feed it to any ROM manager, or to
        // Import once the files are found.
        out << "<?xml version=\"1.0\"?>\n"
            << "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" \"http://www.logiqx.com/Dats/datafile.dtd\">\n"
            << "<datafile>\n\t<header>\n\t\t<name>Bootcade - Missing sets</name>\n"
            << "\t\t<description>Sets missing or incorrect in the library, exported by Bootcade</description>\n"
            << "\t\t<version>" << format_time(std::time(nullptr)) << "</version>\n\t</header>\n";
        for (const auto& g : m_audit.games) {
            if (g.status == "available" || g.ignored) continue;
            out << "\t<game name=\"" << xml(g.name) << "\"";
            if (!g.cloneof.empty()) out << " cloneof=\"" << xml(g.cloneof) << "\" romof=\"" << xml(g.cloneof) << "\"";
            out << ">\n\t\t<description>" << xml(g.description) << "</description>\n";
            for (const auto& r : g.roms)
                out << "\t\t<rom name=\"" << xml(r.name) << "\" size=\"" << r.size << "\" crc=\"" << crc_hex(r.crc) << "\""
                    << (r.state == RomAudit::RomState::Present ? " status=\"verified\"" : "") << "/>\n";
            out << "\t</game>\n";
        }
        out << "</datafile>\n";
    }
    flash(Glib::ustring::compose(_("Exported to %1."), dlg.get_filename()));
}

// ═══ Worker plumbing ════════════════════════════════════════════════════════

RomInbox::Callbacks RomLibraryTab::make_callbacks() {
    RomInbox::Callbacks cb;
    cb.progress  = [this](double p, const std::string& m) { push_progress(p, m); };
    cb.log       = [this](const std::string& m) { push_log(m); };
    cb.cancelled = [this] { return m_cancelled.load(); };
    return cb;
}

void RomLibraryTab::push_progress(double pct, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_current_message = msg;
    }
    m_progress_value.store(pct);
    static thread_local auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= UI_DISPATCH_INTERVAL_MS) {
        last = now;
        m_progress_dispatcher();
    }
}

void RomLibraryTab::push_log(const std::string& msg) {
    std::cerr << "[LIBRARY] " << msg << std::endl;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.push_back(msg);
    }
    m_progress_dispatcher();
}

void RomLibraryTab::on_progress_update() {
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
    if (!message.empty() && m_busy) m_status.set_text(message);
    for (const auto& l : pending) m_sig_log.emit(l);
}

void RomLibraryTab::on_worker_finished() {
    if (m_worker.joinable()) m_worker.join();
    on_progress_update();

    if (m_job == Job::Audit) {
        m_audit_ever_run = true;
        m_db->setScanMetadata("last_audit_time", (int64_t)std::time(nullptr));
        update_last_audit_label();
        populate();
    } else if (m_job == Job::Quarantine) {
        Glib::ustring status = Glib::ustring::compose(
            _("Quarantined %1 set(s), cleaned %2 archive(s), sent %3 orphan(s) to Import."),
            m_qjob.moved, m_qjob.cleaned, m_qjob.sent);
        if (m_qjob.failed) status += Glib::ustring::compose(_(" %1 item(s) could not be processed."), m_qjob.failed);
        m_job = Job::None;
        set_busy(false);
        flash(status);
        if (m_qjob.moved || m_qjob.cleaned || m_qjob.sent) m_sig_scan.emit();  // the owner rescans, then calls refresh_after_scan()
        return;
    }
    m_job = Job::None;
    set_busy(false);
}

void RomLibraryTab::set_busy(bool busy) {
    m_busy = busy;
    m_btn_scan->set_sensitive(!busy);
    m_btn_audit->set_sensitive(!busy);
    m_btn_export->set_sensitive(!busy);
    m_btn_select_all->set_sensitive(!busy);
    m_btn_select_none->set_sensitive(!busy);
    if (busy) { m_progress.set_fraction(0.0); m_progress.show(); m_btn_cancel->show(); }
    else      { m_progress.hide(); m_btn_cancel->hide(); }
    update_action_buttons();
}

void RomLibraryTab::flash(const Glib::ustring& text) {
    if (m_busy) return;
    m_status.set_text(text);
    m_flash_timer.disconnect();
    m_flash_timer = Glib::signal_timeout().connect([this] { if (!m_busy) update_summary(); return false; }, 3500);
}
