// src/ControllerDialog.h
#pragma once
#include <gtkmm.h>
#include <string>
#include <vector>
#include "ControllerConfig.h"
#include "ControllerManager.h"

class ControllerDialog : public Gtk::Dialog {
public:
    ControllerDialog(Gtk::Window& parent,
                     ControllerConfig& config,
                     const std::string& config_path);
    ~ControllerDialog();

private:
    ControllerConfig&    m_config;
    std::string          m_config_path;
    std::vector<JoystickInfo> m_devices;

    Gtk::Notebook        m_notebook;

    // Per-player device combo (managed widgets, stored as raw ptrs)
    Gtk::ComboBoxText*   m_device_combos[2]{};

    // Binding labels and bind buttons: indexed by [player * GAME_ACTION_COUNT + action]
    std::vector<Gtk::Label*>  m_binding_labels;
    std::vector<Gtk::Button*> m_bind_buttons;

    // Active "press a button" state
    int              m_bind_fd   = -1;
    sigc::connection m_poll_conn;

    void build_player_tab(int p);
    void refresh_bindings(int p);
    void on_device_changed(int p);
    void start_binding(int p, GameAction action);
    void stop_binding();
    void on_save_clicked();
};
