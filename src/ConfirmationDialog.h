// src/ConfirmationDialog.h
#pragma once

#include <gtkmm.h>
#include <string>

class ConfirmationDialog : public Gtk::Dialog {
public:
    ConfirmationDialog(Gtk::Window& parent, const std::string& title, const std::string& message, const std::string& emoji = "⚠️");
    virtual ~ConfirmationDialog() = default;

    // Returns true if user clicked Continue
    bool show_and_confirm();

private:
    void on_continue_clicked();
    void on_cancel_clicked();
    
    // UI Elements
    Gtk::Box m_content_box{Gtk::ORIENTATION_VERTICAL, 15};
    Gtk::Label m_title_label;
    Gtk::Label m_message_label;
    Gtk::Box m_button_box{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Button m_cancel_button{"Cancel"};
    Gtk::Button m_continue_button{"Continue"};
    
    bool m_confirmed{false};
};