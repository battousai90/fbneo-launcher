// src/LoginDialog.h
//
// Fenêtre de connexion au compte Bootcade.
//
// Elle affiche le code rendu par le serveur, ouvre le navigateur sur une
// adresse où ce code est déjà prérempli, et attend que le joueur valide.
//
// Le code reste affiché même quand le navigateur s'ouvre correctement : c'est
// le seul recours si l'ouverture échoue, si le joueur préfère valider depuis
// son téléphone, ou s'il joue sur une machine sans navigateur commode, ce qui
// est exactement le cas d'un Steam Deck en mode gaming.
#pragma once

#include <gtkmm.h>
#include <atomic>
#include <memory>
#include <string>
#include <ctime>
#include <thread>

#include "BootcadeAuth.h"

class LoginDialog : public Gtk::Dialog {
public:
    explicit LoginDialog(Gtk::Window& parent);
    ~LoginDialog() override;

    // -> true si le joueur s'est connecté.
    bool succeeded() const { return m_ok; }

private:
    void start();                    // demande un code et lance l'attente
    void on_poll_done();             // repasse sur le fil de l'UI
    void finish_ok();

    Gtk::Box     m_box{Gtk::ORIENTATION_VERTICAL, 14};
    Gtk::Label   m_intro;
    Gtk::Label   m_code;             // le code, en gros
    // L'adresse Keycloak complete n'est PAS affichee : c'est un detail
    // d'implementation qui n'apprend rien au joueur. Le bouton suffit,
    // puisqu'il ouvre l'adresse ou le code est deja prerempli.
    Gtk::Label   m_countdown;         // temps restant avant expiration
    Gtk::Button  m_open;
    Gtk::Label   m_status;
    Gtk::Spinner m_spinner;

    BootcadeAuth::DeviceCode m_dc;
    std::thread              m_worker;
    std::atomic<bool>        m_stop{false};
    Glib::Dispatcher         m_done;
    BootcadeAuth::Poll       m_result = BootcadeAuth::Poll::Pending;
    bool                     m_ok = false;
    sigc::connection         m_close_timer;
    sigc::connection         m_tick;
    std::time_t              m_deadline = 0;
};
