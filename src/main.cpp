// src/main.cpp
#include "MainWindow.h"
#include <gtkmm.h>

int main(int argc, char *argv[]) {
    auto app = Gtk::Application::create(argc, argv, "org.gilbert.fbneo-launcher");

    MainWindow window;
    return app->run(window);
}