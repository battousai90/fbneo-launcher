// src/ConfirmationDialog.cpp
#include "ConfirmationDialog.h"
#include "SettingsUi.h"
#include "i18n.h"

ConfirmationDialog::ConfirmationDialog(Gtk::Window& parent, const std::string& title, const std::string& message,
                                       const std::string& icon_file, bool destructive,
                                       const std::string& caution)
    : Gtk::Dialog() {
    namespace ui = SettingsUi;

    set_transient_for(parent);
    set_modal(true);
    set_resizable(false);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    set_default_size(520, -1);

    // La barre de titre de la charte : tuile a pictogramme, titre, croix.
    // La croix repond comme Annuler : fermer une question, c'est y renoncer.
    ui::window_header(*this, icon_file, title, "",
                      [this] { on_cancel_clicked(); });

    m_body.set_margin_start(22);
    m_body.set_margin_end(22);
    m_body.set_margin_top(20);
    m_body.set_margin_bottom(20);

    if (!caution.empty())
        m_body.pack_start(*ui::warning_card(_("WARNING"), caution), Gtk::PACK_SHRINK);

    m_message_label.set_text(message);
    m_message_label.set_xalign(0.0f);
    m_message_label.set_line_wrap(true);
    m_message_label.set_line_wrap_mode(Pango::WRAP_WORD);
    m_message_label.set_max_width_chars(58);
    m_body.pack_start(m_message_label, Gtk::PACK_EXPAND_WIDGET);
    m_content_box.pack_start(m_body, Gtk::PACK_EXPAND_WIDGET);

    // Les actions en bas a DROITE, la principale en dernier : la convention
    // de tous les ecrans de l'application.
    auto* foot = ui::footer();
    m_continue_button = ui::button(destructive ? _("Delete") : _("Continue"), "",
                                   destructive ? ui::Tone::Danger : ui::Tone::Accent);
    m_cancel_button = ui::button(_("Cancel"));
    m_continue_button->set_size_request(110, -1);
    m_cancel_button->set_size_request(96, -1);
    foot->pack_end(*m_continue_button, Gtk::PACK_SHRINK);
    foot->pack_end(*m_cancel_button, Gtk::PACK_SHRINK);
    m_content_box.pack_start(*foot, Gtk::PACK_SHRINK);

    get_content_area()->set_spacing(0);
    get_content_area()->pack_start(m_content_box, Gtk::PACK_EXPAND_WIDGET);

    m_cancel_button->signal_clicked().connect(sigc::mem_fun(*this, &ConfirmationDialog::on_cancel_clicked));
    m_continue_button->signal_clicked().connect(sigc::mem_fun(*this, &ConfirmationDialog::on_continue_clicked));

    show_all_children();
    // Le geste sur : la touche Entree ne doit pas lancer un traitement long.
    m_cancel_button->set_can_default(true);
    m_cancel_button->grab_default();
    m_cancel_button->grab_focus();
}

bool ConfirmationDialog::show_and_confirm() {
    m_confirmed = false;
    run();
    // Gone as soon as answered : the caller often runs a long operation (DAT
    // update, scan) before this object goes out of scope, and the question
    // stayed on screen behind the progress window the whole time.
    hide();
    return m_confirmed;
}

void ConfirmationDialog::on_continue_clicked() {
    m_confirmed = true;
    response(Gtk::RESPONSE_OK);
}

void ConfirmationDialog::on_cancel_clicked() {
    m_confirmed = false;
    response(Gtk::RESPONSE_CANCEL);
}
