// src/GenerateDAT.h
#pragma once

#include <gtkmm.h>
#include <string>

class GenerateDAT {
public:
    static void execute(Gtk::Window& parent, const std::string& fbneo_executable, Gtk::Entry* dat_entry = nullptr);

private:
    static void show_success_dialog(Gtk::Window& parent, const std::string& dat_path, Gtk::Entry* dat_entry);
};