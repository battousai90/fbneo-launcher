// src/ControllerDialog.h
#pragma once
#include <gtkmm.h>
#include <string>
#include <vector>
#include <map>
#include "ControllerConfig.h"
#include "ControllerManager.h"

class ControllerDialog : public Gtk::Dialog {
public:
    // Takes a copy of all profiles + the active profile name.
    // On Save, writes everything to config_path.
    /* Pas de parent : c'est une fenetre a part entiere, deplacable sur un
     * autre ecran pendant que le launcher reste ou il est. La lier par
     * transient_for la collait a lui. */
    ControllerDialog(const std::map<std::string, ControllerConfig>& profiles,
                     const std::string& active_profile,
                     const std::string& config_path);
    ~ControllerDialog();

private:
    // ── Profile state ─────────────────────────────────────────────────────
    std::map<std::string, ControllerConfig> m_profiles;
    std::string                             m_active_profile_name;
    std::string                             m_config_path;
    bool                                    m_profile_switching{false};

    // Working copy of the active profile
    ControllerConfig m_config;

    // ── Profile bar widgets ───────────────────────────────────────────────
    Gtk::Box          m_profile_bar{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Label        m_profile_label;
    Gtk::ComboBoxText m_profile_combo;
    // Les trois gestes rares passent derriere un menu, voir build_profile_bar.
    Gtk::MenuButton   m_btn_profile_more;
    Gtk::Menu         m_profile_menu;
    Gtk::MenuItem     m_mi_new, m_mi_rename, m_mi_delete;
    Gtk::Button       m_btn_new;      // conserves : encore references ailleurs
    Gtk::Button       m_btn_rename;
    Gtk::Button       m_btn_delete;

    // ── Player tabs ───────────────────────────────────────────────────────
    Gtk::Notebook              m_notebook;
    std::vector<JoystickInfo>  m_devices;
    Gtk::ComboBoxText*         m_device_combos[2]{};

    // Binding labels / buttons: indexed [player * GAME_ACTION_COUNT + action]
    std::vector<Gtk::Label*>   m_binding_labels;
    std::vector<Gtk::Button*>  m_bind_buttons;

    // Active "press a button" state
    int              m_bind_fd  = -1;
    sigc::connection m_poll_conn;

    // ── UI builders ───────────────────────────────────────────────────────
    void build_profile_bar();
    void build_player_tab(int p);

    // ── Analog tab ────────────────────────────────────────────────────────
    // One page for the controls that are not buttons: wheels, paddles, dials,
    // pedals, pointers. Separate from the player tabs because these are
    // per-role rather than per-direction, and because the page is where a
    // mouse or a light gun will be added without disturbing anything else.
    // Un bouton qui dit a quoi la commande est liee, et une case pour
    // inverser. Le reste, numero d'axe et mode, se deduit de ce que le joueur
    // bouge ou tape : lui demander etait lui faire faire le travail du code.
    struct AnalogWidgets {
        Gtk::Button*      bind   = nullptr;
        Gtk::CheckButton* invert = nullptr;
        AnalogBinding     value;               // ce qui est lie aujourd'hui
    };
    AnalogWidgets m_analog_widgets[2][ANALOG_ROLE_COUNT];
    bool          m_analog_loading = false;    // suppresses signals while filling
    // Built inside each player tab, not as a page of its own: these controls
    // belong to a player exactly like their buttons do, and a separate tab
    // duplicated the Player 1 / Player 2 split that already existed.
    Gtk::Widget* build_analog_section(int p);
    void analog_ui_from_config();
    void capture_analog(int player, AnalogRole role);
    void analog_config_from_ui();

    // ── Profile management ────────────────────────────────────────────────
    void populate_profile_combo();
    void save_active_to_profiles();   // sync m_config → m_profiles[m_active]
    void load_profile(const std::string& name); // sync m_profiles[name] → m_config + refresh UI

    // "Press a button on the pad you want." Two identical-looking pads are
    // indistinguishable from their names alone : /dev/input/js0 and js1 say
    // nothing about which one is in your hands.
    void identify_device(int p);

    void on_profile_changed();
    void on_new_profile_clicked();
    void on_rename_profile_clicked();
    void on_delete_profile_clicked();

    // ── Binding helpers ───────────────────────────────────────────────────
    void refresh_bindings(int p);
    void refresh_from_config();       // update device combos + all labels from m_config
    void on_device_changed(int p);
    /* Assistant sequentiel : les douze commandes, dans l'ordre.
     *
     * Il travaille sur une COPIE et ne l'applique qu'a la fin. Ecrire au fur
     * et a mesure dans le profil actif detruirait une configuration existante
     * des que le joueur abandonne en cours de route, ce qui est le geste le
     * plus probable quand on decouvre l'ecran.
     */
    void run_auto_configure(int p);

    void start_binding(int p, GameAction action);
    void stop_binding();
    void on_save_clicked();
};
