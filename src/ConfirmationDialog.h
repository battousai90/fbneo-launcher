// src/ConfirmationDialog.h
#pragma once

#include <gtkmm.h>
#include <string>

/* La question posee avant un traitement, dans le langage visuel de Bootcade.
 *
 * En-tete a tuile (SettingsUi::window_header), corps en texte courant, et,
 * quand il y a une vraie mise en garde, la carte ambre de la fiche de jeu :
 * le SEUL dessin que l'application donne a un avertissement. Elle portait
 * jusqu'ici un emoji dans son titre, ce qui faisait deux langages pour la
 * meme chose selon l'ecran ou l'on se trouvait.
 */
class ConfirmationDialog : public Gtk::Dialog {
public:
    // `icon_file` : le trace de assets/icons qui dit de QUOI il s'agit
    // (bc-search.svg pour un scan, bc-sync.svg pour un rechargement...).
    // `caution` : ce qui merite l'ambre ; vide, aucune carte n'est posee.
    ConfirmationDialog(Gtk::Window& parent, const std::string& title, const std::string& message,
                        const std::string& icon_file = "bc-warning.svg",
                        bool destructive = false, const std::string& caution = "");
    virtual ~ConfirmationDialog() = default;

    // Returns true if user clicked Continue
    bool show_and_confirm();

private:
    void on_continue_clicked();
    void on_cancel_clicked();

    Gtk::Box     m_content_box{Gtk::ORIENTATION_VERTICAL, 0};
    Gtk::Box     m_body{Gtk::ORIENTATION_VERTICAL, 14};
    Gtk::Label   m_message_label;
    Gtk::Button* m_cancel_button   = nullptr;
    Gtk::Button* m_continue_button = nullptr;

    bool m_confirmed{false};
};
