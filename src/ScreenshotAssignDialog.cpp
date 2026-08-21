// src/ScreenshotAssignDialog.cpp
#include "ScreenshotAssignDialog.h"
#include "i18n.h"
#include <filesystem>

ScreenshotAssignDialog::ScreenshotAssignDialog(Gtk::Window& parent, const std::vector<std::string>& screenshot_paths)
    : Gtk::Dialog(_("Use a captured screenshot as artwork?"), parent, true) {
    set_default_size(520, 360);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    get_content_area()->pack_start(m_box, Gtk::PACK_EXPAND_WIDGET);
    m_box.set_margin_start(16);
    m_box.set_margin_end(16);
    m_box.set_margin_top(14);
    m_box.set_margin_bottom(10);

    m_hint.set_text(_("F6 was pressed during this session. Tick which capture (if any) should become the Title and/or Preview artwork for this game."));
    m_hint.set_line_wrap(true);
    m_hint.set_halign(Gtk::ALIGN_START);
    m_box.pack_start(m_hint, Gtk::PACK_SHRINK);

    m_model = Gtk::ListStore::create(m_columns);
    for (const auto& path : screenshot_paths) {
        auto row = *m_model->append();
        try {
            row[m_columns.thumb] = Gdk::Pixbuf::create_from_file(path, 96, 72, true);
        } catch (...) {
            row[m_columns.thumb] = Glib::RefPtr<Gdk::Pixbuf>();
        }
        row[m_columns.filename] = Glib::ustring(std::filesystem::path(path).filename().string());
        row[m_columns.full_path] = path;
        row[m_columns.is_title] = false;
        row[m_columns.is_preview] = false;
    }

    m_view.set_model(m_model);
    m_view.append_column("", m_columns.thumb);
    m_view.append_column(_("File"), m_columns.filename);

    auto* title_renderer = Gtk::make_managed<Gtk::CellRendererToggle>();
    title_renderer->set_radio(true);
    title_renderer->signal_toggled().connect(sigc::mem_fun(*this, &ScreenshotAssignDialog::on_title_toggled));
    int title_col = m_view.append_column(_("Title"), *title_renderer) - 1;
    if (auto* c = m_view.get_column(title_col)) c->add_attribute(title_renderer->property_active(), m_columns.is_title);

    auto* preview_renderer = Gtk::make_managed<Gtk::CellRendererToggle>();
    preview_renderer->set_radio(true);
    preview_renderer->signal_toggled().connect(sigc::mem_fun(*this, &ScreenshotAssignDialog::on_preview_toggled));
    int preview_col = m_view.append_column(_("Preview"), *preview_renderer) - 1;
    if (auto* c = m_view.get_column(preview_col)) c->add_attribute(preview_renderer->property_active(), m_columns.is_preview);

    m_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    m_scroll.add(m_view);
    m_box.pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);

    add_button(_("Skip"), Gtk::RESPONSE_CANCEL);
    auto* save_button = add_button(_("Save"), Gtk::RESPONSE_OK);
    save_button->get_style_context()->add_class("accent-button");
    signal_response().connect([this](int response_id) {
        if (response_id == Gtk::RESPONSE_OK) on_save_clicked();
    });

    show_all_children();
}

void ScreenshotAssignDialog::on_title_toggled(const Glib::ustring& path) {
    // Radio-like: only one row can be "the" title.
    Gtk::TreePath toggled_path(path);
    for (auto& row : m_model->children()) {
        row[m_columns.is_title] = (m_model->get_path(row) == toggled_path) ? !row[m_columns.is_title] : false;
    }
}

void ScreenshotAssignDialog::on_preview_toggled(const Glib::ustring& path) {
    Gtk::TreePath toggled_path(path);
    for (auto& row : m_model->children()) {
        row[m_columns.is_preview] = (m_model->get_path(row) == toggled_path) ? !row[m_columns.is_preview] : false;
    }
}

void ScreenshotAssignDialog::on_save_clicked() {
    for (auto& row : m_model->children()) {
        if (row[m_columns.is_title]) m_title_choice = row[m_columns.full_path];
        if (row[m_columns.is_preview]) m_preview_choice = row[m_columns.full_path];
    }
}
