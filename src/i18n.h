// src/i18n.h : lightweight runtime internationalization.
//
// Usage: wrap user-visible English source strings with _() e.g. _("Scan ROMs").
// English is the source language and the fallback: if the active language has no
// translation for a key, the English key itself is returned. Translations live in
// locale/<lang>.json as a flat { "English string": "translation" } map : adding a
// language is just dropping in a new JSON file, no code or rebuild of logic needed.
#pragma once
#include <string>
#include <vector>

namespace i18n {
    // Load catalogs from locale_dir. If lang is empty, auto-detect from the
    // environment (LANGUAGE/LC_ALL/LC_MESSAGES/LANG), falling back to English.
    void init(const std::string& locale_dir, const std::string& lang = "");

    // Switch the active language at runtime (reloads its catalog).
    void set_language(const std::string& lang);
    const std::string& language();

    // Translate a source string (returns the key itself if untranslated).
    std::string tr(const std::string& key);

    // "en" plus every <lang>.json found in locale_dir.
    std::vector<std::string> available_languages();
}

// gettext-style shorthand. Function-like macro: only expands on `_(` , so a bare
// `_` used as an identifier (e.g. a structured-binding placeholder) is unaffected.
#ifndef _
#define _(s) i18n::tr(s)
#endif
// Mark a string for translation without translating it at this point.
#define N_(s) (s)
