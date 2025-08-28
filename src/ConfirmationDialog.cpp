// src/ConfirmationDialog.cpp
#include "ConfirmationDialog.h"

ConfirmationDialog::ConfirmationDialog(Gtk::Window& parent, const std::string& title, const std::string& message, const std::string& emoji)
    : Gtk::Dialog(emoji + " " + title, parent, true) {
    
    set_size_request(500, 200);
    set_resizable(false);
    set_modal(true);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    
    // Setup content area with same styling as DownloadDialog
    get_content_area()->pack_start(m_content_box, Gtk::PACK_EXPAND_WIDGET);
    m_content_box.set_spacing(15);
    m_content_box.set_margin_start(25);
    m_content_box.set_margin_end(25);
    m_content_box.set_margin_top(20);
    m_content_box.set_margin_bottom(20);
    
    // Title with emoji and styling
    m_title_label.set_markup("<span size='x-large' weight='bold'>" + emoji + " " + title + "</span>");
    m_title_label.set_halign(Gtk::ALIGN_CENTER);
    m_title_label.set_margin_bottom(10);
    m_content_box.pack_start(m_title_label, Gtk::PACK_SHRINK);
    
    // Message with better formatting
    m_message_label.set_markup("<span size='medium'>" + message + "</span>");
    m_message_label.set_halign(Gtk::ALIGN_START);
    m_message_label.set_line_wrap(true);
    m_message_label.set_line_wrap_mode(Pango::WRAP_WORD);
    m_message_label.set_justify(Gtk::JUSTIFY_LEFT);
    m_content_box.pack_start(m_message_label, Gtk::PACK_EXPAND_WIDGET);
    
    // Button box with styling
    m_button_box.set_halign(Gtk::ALIGN_END);
    m_button_box.set_margin_top(10);
    
    // Style buttons like in other dialogs
    m_cancel_button.set_size_request(80, 32);
    m_continue_button.set_size_request(80, 32);
    
    // Pack buttons
    m_button_box.pack_start(m_cancel_button, Gtk::PACK_SHRINK);
    m_button_box.pack_start(m_continue_button, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_button_box, Gtk::PACK_SHRINK);
    
    // Connect signals
    m_cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &ConfirmationDialog::on_cancel_clicked));
    m_continue_button.signal_clicked().connect(sigc::mem_fun(*this, &ConfirmationDialog::on_continue_clicked));
    
    // Set default button (Cancel is safer)
    m_cancel_button.set_can_default(true);
    m_cancel_button.grab_default();
    
    show_all_children();
}

bool ConfirmationDialog::show_and_confirm() {
    m_confirmed = false;
    run();
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