// src/SettingsUi.cpp
#include "SettingsUi.h"

#include "IconManager.h"
#include "i18n.h"

#include <fstream>

namespace SettingsUi {

Gtk::Image* image(const std::string& icon_file, int size) {
    auto* img = Gtk::make_managed<Gtk::Image>(
        IconManager::load("icons/" + icon_file, size, size));
    img->set_halign(Gtk::ALIGN_CENTER);
    img->set_valign(Gtk::ALIGN_CENTER);
    return img;
}

/* Le pictogramme est centre par la GEOMETRIE de sa tuile, jamais par des
 * marges : c'est ce qui garantit que douze lignes d'une page ont leurs icones
 * exactement sur la meme verticale, quelle que soit la taille du trace. */
Gtk::Widget* tile(const std::string& icon_file, int icon_size, int box_size,
                  bool accent) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    box->set_size_request(box_size, box_size);
    box->pack_start(*image(icon_file, icon_size), Gtk::PACK_EXPAND_WIDGET);
    box->get_style_context()->add_class(accent ? "set-brand" : "set-tile");
    box->set_halign(Gtk::ALIGN_CENTER);
    box->set_valign(Gtk::ALIGN_CENTER);
    return box;
}

Gtk::Label* title_label(const std::string& text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->get_style_context()->add_class("set-row-title");
    return l;
}

Gtk::Label* sub_label(const std::string& text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->set_line_wrap(true);
    l->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    l->get_style_context()->add_class("set-sub");
    return l;
}

Gtk::Label* card_title_label(const std::string& text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->get_style_context()->add_class("set-card-title");
    return l;
}

Gdk::RGBA probe_color(Gtk::Container& host, const std::string& css_class) {
    auto* probe = Gtk::make_managed<Gtk::Label>();
    probe->get_style_context()->add_class(css_class);
    probe->set_no_show_all(true);
    host.add(*probe);
    Gdk::RGBA colour = probe->get_style_context()->get_color(Gtk::STATE_FLAG_NORMAL);
    host.remove(*probe);
    return colour;
}

Gtk::Widget* hairline() {
    auto* sep = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    sep->set_size_request(-1, 1);
    sep->get_style_context()->add_class("set-rowsep");
    return sep;
}

Card card(const std::string& icon_file, const std::string& title,
          const std::string& subtitle) {
    Card c;
    c.frame = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 14);
    c.frame->get_style_context()->add_class("cc-card");

    c.head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 13);
    c.head->pack_start(*tile(icon_file, kIconSection, kTileSection),
                       Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
    txt->set_valign(Gtk::ALIGN_CENTER);
    c.title = card_title_label(title);
    txt->pack_start(*c.title, Gtk::PACK_SHRINK);
    if (!subtitle.empty()) {
        c.subtitle = sub_label(subtitle);
        txt->pack_start(*c.subtitle, Gtk::PACK_SHRINK);
    }
    c.head->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    c.frame->pack_start(*c.head, Gtk::PACK_SHRINK);

    c.body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    c.frame->pack_start(*c.body, Gtk::PACK_EXPAND_WIDGET);
    /* La carte s'etire en hauteur, elle ne se retracte pas.
     *
     * Sur une page a deux colonnes, la colonne la plus courte doit descendre
     * jusqu'au bas de l'autre : « Your Profile » s'aligne sur « Network
     * Status », et le cadre des emulateurs sur celui des options. Deux
     * colonnes qui s'arretent a des hauteurs differentes se lisent comme un
     * defaut d'alignement, pas comme une intention.
     *
     * Le contenu, lui, reste en haut de la carte : c'est le CADRE qui
     * descend, pas les lignes qui s'espacent.
     */
    return c;
}

Gtk::Box* rows() {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    box->get_style_context()->add_class("set-rows");
    return box;
}

/* Le filet n'apparait qu'a partir de la deuxieme ligne : un trait au-dessus de
 * la premiere doublerait la bordure du cadre. */
void add_row(Gtk::Box* container, Gtk::Widget& row) {
    if (!container->get_children().empty())
        container->pack_start(*hairline(), Gtk::PACK_SHRINK);
    container->pack_start(row, Gtk::PACK_SHRINK);
}

Gtk::Widget* row(const std::string& icon_file, const std::string& title,
                 const std::string& subtitle, Gtk::Widget* trailing) {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 13);
    line->get_style_context()->add_class("set-row");

    if (!icon_file.empty())
        line->pack_start(*tile(icon_file, kIconRow, kTileRow), Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*title_label(title), Gtk::PACK_SHRINK);
    if (!subtitle.empty()) txt->pack_start(*sub_label(subtitle), Gtk::PACK_SHRINK);
    line->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);

    if (trailing) {
        trailing->set_valign(Gtk::ALIGN_CENTER);
        line->pack_start(*trailing, Gtk::PACK_SHRINK);
    }
    return line;
}

Gtk::Button* button(const std::string& label, const std::string& icon_file,
                    Tone tone) {
    auto* b = Gtk::make_managed<Gtk::Button>(label);
    if (!icon_file.empty()) {
        b->set_image(*image(icon_file, kIconButton));
        b->set_always_show_image(true);
    }
    if (tone == Tone::Accent) b->get_style_context()->add_class("accent-button");
    if (tone == Tone::Danger) b->get_style_context()->add_class("set-danger");
    b->set_valign(Gtk::ALIGN_CENTER);
    return b;
}

namespace {
const char* state_class(State s) {
    switch (s) {
        case State::Ok:    return "set-ok";
        case State::Warn:  return "set-warn";
        case State::Error: return "set-err";
        default:           return "set-sub";
    }
}
}  // namespace

Gtk::Label* status_label(const std::string& text, State state) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->get_style_context()->add_class(state_class(state));
    return l;
}

void notice(Gtk::Window& parent, const std::string& title,
            const std::string& message, const std::string& icon_file) {
    Gtk::Dialog dlg;
    // Un titre meme sans barre de titre : c'est ce que lisent le gestionnaire
    // de fenetres, la barre des taches et les outils d'accessibilite.
    dlg.set_title(title);
    dlg.set_transient_for(parent);
    dlg.set_modal(true);
    dlg.set_resizable(false);
    // Pas de barre de titre : la boite porte son titre dans son contenu, comme
    // les cartes. Une barre du bureau au-dessus ferait exactement le melange
    // qu'on cherche a eviter.
    dlg.set_decorated(false);
    dlg.get_style_context()->add_class("cc-window");
    dlg.get_style_context()->add_class("set-window");
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* content = dlg.get_content_area();
    content->set_spacing(0);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 15);
    body->get_style_context()->add_class("set-notice");
    body->pack_start(*tile(icon_file, kIconSection, kTileSection), Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*card_title_label(title), Gtk::PACK_SHRINK);
    if (!message.empty()) {
        auto* sub = sub_label(message);
        sub->set_max_width_chars(46);
        txt->pack_start(*sub, Gtk::PACK_SHRINK);
    }
    body->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    content->pack_start(*body, Gtk::PACK_EXPAND_WIDGET);

    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    foot->get_style_context()->add_class("cc-footer");
    auto* ok = button(_("OK"), "", Tone::Accent);
    ok->set_size_request(96, -1);
    ok->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_OK); });
    foot->pack_end(*ok, Gtk::PACK_SHRINK);
    content->pack_start(*foot, Gtk::PACK_SHRINK);

    dlg.show_all_children();
    ok->grab_focus();
    dlg.run();
}

Gtk::Widget* status_dot(const std::string& text, State state) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* dot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    dot->get_style_context()->add_class("set-dot");
    dot->get_style_context()->add_class(state_class(state));
    dot->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*dot, Gtk::PACK_SHRINK);
    box->pack_start(*status_label(text, state), Gtk::PACK_SHRINK);
    box->set_valign(Gtk::ALIGN_CENTER);
    return box;
}

// ═══ Les briques des ecrans de donnees ═════════════════════════════════════

namespace {
const char* pill_tone_class(PillTone t) {
    switch (t) {
        case PillTone::Accent: return "set-accent";
        case PillTone::Ok:     return "set-ok";
        case PillTone::Warn:   return "set-warn";
        case PillTone::Error:  return "set-err";
        default:               return "set-off";
    }
}

// « 29 519 » : l'espace fine entre les milliers, comme partout dans
// l'application.
std::string thousands(long n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int k = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it, ++k) {
        if (k && k % 3 == 0) out.insert(0, "\u202f");
        out.insert(0, 1, *it);
    }
    return (n < 0 ? "-" : "") + out;
}
}  // namespace

// ── Pill ───────────────────────────────────────────────────────────────────

Pill::Pill(const std::string& label, PillTone tone, bool filter)
    : Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0), m_tone(tone), m_filter(filter) {
    auto* inner = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    m_label.set_text(label);
    m_label.set_xalign(0.0f);
    m_count.get_style_context()->add_class("set-pill-count");
    m_count.set_no_show_all(true);
    inner->pack_start(m_label, Gtk::PACK_SHRINK);
    inner->pack_start(m_count, Gtk::PACK_SHRINK);

    if (m_filter) {
        // Le bouton EST la pastille : c'est lui qui porte les classes, pour
        // que l'etat coche / decoche pilote la teinte.
        m_button = Gtk::make_managed<Gtk::ToggleButton>();
        m_button->add(*inner);
        m_button->set_can_focus(false);
        m_button->signal_toggled().connect([this] { m_toggled.emit(m_button->get_active()); });
        m_face = m_button;
        pack_start(*m_button, Gtk::PACK_SHRINK);
    } else {
        m_face = inner;
        pack_start(*inner, Gtk::PACK_SHRINK);
    }
    m_face->get_style_context()->add_class("set-pill");
    if (m_filter) m_face->get_style_context()->add_class("set-filter");
    set_valign(Gtk::ALIGN_CENTER);
    apply_tone();
    show_all_children();
}

void Pill::apply_tone() {
    auto ctx = m_face->get_style_context();
    for (const char* c : {"set-off", "set-accent", "set-ok", "set-warn", "set-err"})
        ctx->remove_class(c);
    ctx->add_class(pill_tone_class(m_tone));
}

void Pill::set_label(const std::string& text) { m_label.set_text(text); }
void Pill::set_count(long count) { m_count.set_text(thousands(count)); m_count.show(); }
void Pill::clear_count()         { m_count.hide(); }
void Pill::set_tone(PillTone tone) { m_tone = tone; apply_tone(); }

bool Pill::active() const  { return m_button && m_button->get_active(); }
void Pill::set_active(bool on) { if (m_button) m_button->set_active(on); }

// ── FilterBar ──────────────────────────────────────────────────────────────

FilterBar::FilterBar(const std::string& search_placeholder)
    : Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12) {
    m_entry.set_placeholder_text(search_placeholder);
    m_entry.set_icon_from_pixbuf(IconManager::load("icons/bc-search.svg", 16, 16),
                                 Gtk::ENTRY_ICON_PRIMARY);
    m_entry.set_size_request(kFieldWidth, -1);
    m_entry.signal_changed().connect(sigc::mem_fun(*this, &FilterBar::schedule));
    pack_start(m_entry, Gtk::PACK_SHRINK);

    m_summary.set_xalign(1.0f);
    m_summary.get_style_context()->add_class("set-sub");
    pack_end(m_summary, Gtk::PACK_SHRINK);
    set_valign(Gtk::ALIGN_CENTER);
}

// La saisie attend que les doigts s'arretent : refiltrer une table de
// 29 000 lignes a chaque touche la rendrait poisseuse.
void FilterBar::schedule() {
    m_pending.disconnect();
    m_pending = Glib::signal_timeout().connect([this] { m_changed.emit(); return false; }, 220);
}

Gtk::ComboBoxText* FilterBar::add_combo(const std::string& label) {
    auto* group = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->get_style_context()->add_class("set-sub");
    group->pack_start(*l, Gtk::PACK_SHRINK);
    auto* combo = Gtk::make_managed<Gtk::ComboBoxText>();
    combo->signal_changed().connect([this] { m_changed.emit(); });
    group->pack_start(*combo, Gtk::PACK_SHRINK);
    group->set_valign(Gtk::ALIGN_CENTER);
    pack_start(*group, Gtk::PACK_SHRINK);
    group->show_all();
    return combo;
}

std::string FilterBar::search_text() const { return m_entry.get_text().raw(); }
void FilterBar::set_summary(const std::string& text) { m_summary.set_text(text); }

// ── Table ──────────────────────────────────────────────────────────────────

Table::Table(Gtk::SelectionMode mode) : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0) {
    get_style_context()->add_class("set-table");
    m_view.get_selection()->set_mode(mode);
    m_view.set_headers_visible(true);
    m_view.set_enable_search(false);   // la recherche est celle de la barre de filtres
    m_view.signal_button_press_event().connect(sigc::mem_fun(*this, &Table::on_button_press), false);
    m_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scroll.add(m_view);
    pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);
}

bool Table::on_button_press(GdkEventButton* event) {
    if (event->type != GDK_BUTTON_PRESS || event->button != 3) return false;
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cx = 0, cy = 0;
    if (!m_view.get_path_at_pos((int)event->x, (int)event->y, path, column, cx, cy)) return false;
    // Un clic droit sur une ligne deja dans la selection ne la defait pas :
    // c'est ainsi qu'on demande une action sur plusieurs lignes a la fois.
    auto sel = m_view.get_selection();
    if (!sel->is_selected(path)) {
        sel->unselect_all();
        sel->select(path);
    }
    m_context_menu.emit(path, column, event);
    return true;
}

Gtk::TreeViewColumn* Table::add_text_column(const std::string& title,
                                            const Gtk::TreeModelColumn<Glib::ustring>& column,
                                            const ColumnOptions& options) {
    auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
    renderer->property_xalign() = options.xalign;
    if (options.mono) renderer->property_family() = "monospace";
    if (options.expand) renderer->property_ellipsize() = Pango::ELLIPSIZE_END;

    auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(title, *renderer);
    col->add_attribute(renderer->property_text(), column);
    col->set_resizable(true);
    col->set_expand(options.expand);
    if (options.min_width > 0) col->set_min_width(options.min_width);
    if (options.sortable) col->set_sort_column(column);
    m_view.append_column(*col);
    return col;
}

Gtk::TreeViewColumn* Table::add_check_column(const Gtk::TreeModelColumn<bool>& column,
                                             const sigc::slot<void, const Glib::ustring&>& on_toggled) {
    auto* renderer = Gtk::make_managed<Gtk::CellRendererToggle>();
    renderer->set_activatable(true);
    renderer->signal_toggled().connect(on_toggled);
    auto* col = Gtk::make_managed<Gtk::TreeViewColumn>("", *renderer);
    col->add_attribute(renderer->property_active(), column);
    col->set_resizable(false);
    col->set_expand(false);
    m_view.append_column(*col);
    return col;
}

// ── LogPanel ───────────────────────────────────────────────────────────────

LogPanel::LogPanel(const std::string& title, const std::string& subtitle, int actions)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 10) {
    Card c = card("bc-file.svg", title, subtitle);

    auto* tools = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    tools->set_valign(Gtk::ALIGN_CENTER);
    if (actions & AutoScroll) {
        m_follow = Gtk::make_managed<Gtk::CheckButton>(_("Auto-scroll"));
        m_follow->set_active(true);
        tools->pack_end(*m_follow, Gtk::PACK_SHRINK);
    }
    if (actions & Clear) {
        auto* b = button(_("Clear log"), "bc-clear.svg");
        b->signal_clicked().connect(sigc::mem_fun(*this, &LogPanel::clear));
        tools->pack_end(*b, Gtk::PACK_SHRINK);
    }
    if (actions & Export) {
        auto* b = button(_("Export log"), "bc-save.svg");
        b->signal_clicked().connect(sigc::mem_fun(*this, &LogPanel::on_export));
        tools->pack_end(*b, Gtk::PACK_SHRINK);
    }
    if (actions) c.head->pack_end(*tools, Gtk::PACK_SHRINK);

    m_buffer = Gtk::TextBuffer::create();
    m_view.set_buffer(m_buffer);
    m_view.set_editable(false);
    m_view.set_cursor_visible(false);
    m_view.set_wrap_mode(Gtk::WRAP_NONE);
    m_view.get_style_context()->add_class("set-mono");
    m_view.get_style_context()->add_class("set-log");
    m_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scroll.get_style_context()->add_class("set-log-frame");
    m_scroll.add(m_view);
    c.body->pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);
    c.body->set_margin_top(10);
    pack_start(*c.frame, Gtk::PACK_EXPAND_WIDGET);
}

/* Les couleurs des niveaux sont celles des pastilles d'etat, lues dans la
 * feuille de style au moment ou le panneau est dans une fenetre (la regle
 * est « .set-window .set-ok » : detachee, une etiquette ne l'atteint pas).
 * Un TextTag ne se peint pas en CSS, c'est le seul endroit ou une couleur
 * transite par le code : elle n'y est pas ecrite pour autant. */
void LogPanel::ensure_tags() {
    if (m_tags_ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    auto table = m_buffer->get_tag_table();
    struct Probe { const char* tag; const char* css; };
    for (const Probe& p : {Probe{"ok", "set-ok"}, Probe{"warn", "set-warn"},
                           Probe{"error", "set-err"}, Probe{"muted", "set-sub"}}) {
        auto tag = Gtk::TextTag::create(p.tag);
        tag->property_foreground_rgba() = probe_color(*this, p.css);
        if (p.tag == std::string("muted")) tag->property_style() = Pango::STYLE_ITALIC;
        table->add(tag);
    }
    m_tags_ready = true;
}

void LogPanel::append(const std::string& line, Level level) {
    ensure_tags();
    Gtk::TextBuffer::iterator end = m_buffer->end();
    const char* tag = nullptr;
    switch (level) {
        case Level::Ok:    tag = "ok";    break;
        case Level::Warn:  tag = "warn";  break;
        case Level::Error: tag = "error"; break;
        case Level::Muted: tag = "muted"; break;
        default: break;
    }
    if (tag && m_tags_ready) m_buffer->insert_with_tag(end, line + "\n", tag);
    else                     m_buffer->insert(end, line + "\n");
    if (auto_scroll()) {
        auto mark = m_buffer->create_mark(m_buffer->end());
        m_view.scroll_to(mark);
        m_buffer->delete_mark(mark);
    }
}

void LogPanel::clear() { m_buffer->set_text(""); }
std::string LogPanel::text() const { return m_buffer->get_text().raw(); }
void LogPanel::set_auto_scroll(bool on) { if (m_follow) m_follow->set_active(on); else m_follow_default = on; }
bool LogPanel::auto_scroll() const { return m_follow ? m_follow->get_active() : m_follow_default; }

void LogPanel::on_export() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Export log"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Save"),   Gtk::RESPONSE_OK);
    dlg.set_current_name("bootcade-log.txt");
    dlg.set_do_overwrite_confirmation(true);
    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::ofstream out(dlg.get_filename());
    if (!out) {
        if (top) notice(*top, _("Could not write the file."), dlg.get_filename());
        return;
    }
    out << text();
}

// ── DetailPanel ────────────────────────────────────────────────────────────

DetailPanel::DetailPanel(const std::string& icon_file, const std::string& title,
                         const std::string& subtitle, int height)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0) {
    m_card = card(icon_file, title, subtitle);
    m_title = m_card.title;
    m_subtitle = m_card.subtitle;
    m_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scroll.add(m_body);
    m_card.body->pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);
    m_card.body->set_margin_top(8);
    m_card.frame->set_size_request(-1, height);
    pack_start(*m_card.frame, Gtk::PACK_EXPAND_WIDGET);
    show_placeholder("");
}

void DetailPanel::set_title(const std::string& text) { if (m_title) m_title->set_text(text); }
void DetailPanel::set_subtitle(const std::string& text) { if (m_subtitle) m_subtitle->set_text(text); }

void DetailPanel::set_content(Gtk::Widget* content) {
    if (m_content) { m_body.remove(*m_content); m_content = nullptr; }
    if (!content) return;
    m_content = content;
    m_body.pack_start(*content, Gtk::PACK_EXPAND_WIDGET);
    content->show_all();
}

void DetailPanel::show_placeholder(const std::string& message) {
    auto* l = sub_label(message.empty() ? _("Nothing selected.") : message);
    l->set_xalign(0.5f);
    l->set_valign(Gtk::ALIGN_CENTER);
    l->set_margin_top(18);
    set_content(l);
}

}  // namespace SettingsUi
