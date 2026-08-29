// src/HiscoreClient.cpp
#include "HiscoreClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <mutex>

namespace HiscoreClient {
namespace {

std::mutex  g_mutex;
std::string g_base_url;

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// URL-escape a path segment. ROM names are tame, but system names are not:
// "Neo Geo Pocket" and "Fairchild Channel F" both carry spaces, and a raw
// space in a request line is a malformed request.
std::string escape_segment(CURL* curl, const std::string& s) {
    char* enc = curl_easy_escape(curl, s.c_str(), (int)s.size());
    if (!enc) return s;
    std::string out(enc);
    curl_free(enc);
    return out;
}

// GET returning the parsed body, or a null json on any failure.
nlohmann::json get_json(const std::string& path, long timeout_secs) {
    std::string base;
    { std::lock_guard<std::mutex> lock(g_mutex); base = g_base_url; }
    if (base.empty()) return nullptr;

    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, (base + path).c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_secs);
    // Short connect timeout on purpose: when the service is simply not there
    // — the common case away from the homelab — we want to give up quickly
    // and leave the interface alone, not hold a worker thread for the full
    // transfer timeout on every game the player clicks.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade-launcher");

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || status != 200) return nullptr;
    try {
        return nlohmann::json::parse(body);
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace

std::string base_url() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_base_url;
}

void set_base_url(const std::string& url) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_base_url = url;
    // A trailing slash would produce "//api/..." — harmless on most servers,
    // but it defeats any cache keyed on the exact URL.
    while (!g_base_url.empty() && g_base_url.back() == '/') g_base_url.pop_back();
}

std::set<std::string> fetch_supported() {
    std::set<std::string> out;
    // Longer than a per-game lookup: this one is fetched once at startup and
    // the list spans every ranked game.
    auto j = get_json("/api/supported", 15L);
    if (!j.is_array()) return out;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        std::string system = item.value("system", "");
        std::string game   = item.value("game", "");
        if (!system.empty() && !game.empty()) out.insert(key(system, game));
    }
    return out;
}

std::vector<Entry> fetch_top(const std::string& system,
                             const std::string& game,
                             int limit) {
    std::vector<Entry> out;
    if (system.empty() || game.empty()) return out;

    CURL* esc = curl_easy_init();
    if (!esc) return out;
    std::string path = "/api/scores/" + escape_segment(esc, system) + "/" +
                       escape_segment(esc, game) + "/top?limit=" +
                       std::to_string(limit);
    curl_easy_cleanup(esc);

    auto j = get_json(path, 8L);
    if (!j.is_array()) return out;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        Entry e;
        e.player = item.value("player", "");
        if (item.contains("country") && item["country"].is_string())
            e.country = item["country"].get<std::string>();
        if (item.contains("score") && item["score"].is_number())
            e.score = item["score"].get<long long>();
        if (item.contains("since") && item["since"].is_string())
            e.since = item["since"].get<std::string>();
        if (!e.player.empty()) out.push_back(e);
    }
    return out;
}

SubmitResult submit(const std::string& system,
                    const std::string& game,
                    const std::string& player,
                    const std::string& country,
                    const std::string& hi_before,
                    const std::string& hi_after) {
    SubmitResult r;
    std::string base;
    { std::lock_guard<std::mutex> lock(g_mutex); base = g_base_url; }
    if (base.empty() || system.empty() || game.empty() || player.empty()) {
        r.error = "not configured";
        return r;
    }

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    curl_mime* mime = curl_mime_init(curl);
    auto add_field = [&](const char* name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), value.size());
    };
    auto add_file = [&](const char* name, const std::string& bytes) {
        if (bytes.empty()) return;
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_filename(part, name);
        curl_mime_data(part, bytes.data(), bytes.size());
    };
    add_field("system", system);
    add_field("game",   game);
    add_field("player", player);
    if (!country.empty()) add_field("country", country);
    add_file("hi",        hi_after);
    add_file("hi_before", hi_before);

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, (base + "/api/submit").c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade-launcher");

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { r.error = curl_easy_strerror(res); return r; }
    r.reached = true;

    try {
        auto j = nlohmann::json::parse(body);
        // 409 carries a real explanation (a clone has no leaderboard) that the
        // player deserves to read, so it is parsed like any other answer.
        if (j.contains("error"))  r.reason = j.value("detail", j.value("error", ""));
        if (j.contains("reason")) r.reason = j.value("reason", "");
        std::string st = j.value("status", "");
        r.accepted = (st == "accepted");
        r.pending  = (st == "pending");
        if (j.contains("score") && j["score"].is_number()) {
            r.score = j["score"].get<long long>();
            r.has_score = true;
        }
    } catch (const std::exception& e) {
        r.error = std::string("bad reply: ") + e.what();
        r.reached = false;
    }
    if (status >= 500) { r.reached = false; r.error = "HTTP " + std::to_string(status); }
    return r;
}

} // namespace HiscoreClient
