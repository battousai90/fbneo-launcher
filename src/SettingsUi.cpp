// src/SettingsUi.cpp
#include "SettingsUi.h"

#include "IconManager.h"

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
    txt->pack_start(*card_title_label(title), Gtk::PACK_SHRINK);
    if (!subtitle.empty())
        txt->pack_start(*sub_label(subtitle), Gtk::PACK_SHRINK);
    c.head->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    c.frame->pack_start(*c.head, Gtk::PACK_SHRINK);

    c.body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    c.frame->pack_start(*c.body, Gtk::PACK_EXPAND_WIDGET);
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

}  // namespace SettingsUi
