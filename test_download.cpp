#include "src/DownloadDialog.h"
#include <gtkmm.h>
#include <iostream>

int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create(argc, argv, "org.fbneo.test");
    
    auto window = std::make_unique<Gtk::Window>();
    window->set_default_size(400, 300);
    window->set_title("Test Download");
    
    auto download_dialog = std::make_unique<DownloadDialog>(
        *window,
        "https://github.com/finalburnneo/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        "/tmp"
    );
    
    download_dialog->set_success_callback([](const std::string& path) {
        std::cout << "Downloaded to: " << path << std::endl;
    });
    
    download_dialog->show_all();
    download_dialog->start_download();
    
    return download_dialog->run();
}