// src/HiscoreClient.cpp
#include "HiscoreClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

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

namespace {
// Shared by fetch_top and fetch_boards: the two endpoints return the same
// row shape, so they parse the same way.
Entry parse_entry(const nlohmann::json& item) {
    Entry e;
    e.player = item.value("player", "");
    if (item.contains("country") && item["country"].is_string())
        e.country = item["country"].get<std::string>();
    if (item.contains("score") && item["score"].is_number())
        e.score = item["score"].get<long long>();
    if (item.contains("since") && item["since"].is_string())
        e.since = item["since"].get<std::string>();
    return e;
}
} // namespace

std::vector<Board> fetch_boards(int limit) {
    std::vector<Board> out;
    // Longer than a per-game lookup: this covers every ranked game at once.
    auto j = get_json("/api/boards?limit=" + std::to_string(limit), 20L);
    if (!j.is_array()) return out;
    for (const auto& b : j) {
        if (!b.is_object() || !b.contains("rows")) continue;
        Board board;
        board.system = b.value("system", "");
        board.game   = b.value("game", "");
        if (board.system.empty() || board.game.empty()) continue;
        for (const auto& item : b["rows"]) {
            Entry e = parse_entry(item);
            if (!e.player.empty()) board.rows.push_back(e);
        }
        out.push_back(std::move(board));
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
        Entry e = parse_entry(item);
        if (!e.player.empty()) out.push_back(e);
    }
    return out;
}

SubmitResult submit(const std::string& system,
                    const std::string& game,
                    const std::string& player,
                    const std::string& country,
                    const Playtime&    playtime,
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
    // Sent even when the session set no record: a game that was played for
    // twenty minutes was played, whether or not the table moved.
    if (playtime.total > 0) {
        add_field("play_last",    std::to_string(playtime.last));
        add_field("play_longest", std::to_string(playtime.longest));
        add_field("play_total",   std::to_string(playtime.total));
    }
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
        r.ignored  = (st == "ignored" || st == "playtime");
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

// ── Offline store ─────────────────────────────────────────────────────────
namespace {

std::string g_store_dir;

std::string store_path(const std::string& leaf) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_store_dir.empty() ? std::string() : g_store_dir + "/" + leaf;
}

std::string to_hex(const std::string& raw) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) { out += digits[c >> 4]; out += digits[c & 0xF]; }
    return out;
}

std::string from_hex(const std::string& hex) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out += (char)((hi << 4) | lo);
    }
    return out;
}

std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream fi(path);
    if (!fi) return nlohmann::json::object();
    try { nlohmann::json j; fi >> j; return j; }
    catch (const std::exception&) { return nlohmann::json::object(); }
}

void write_json_file(const std::string& path, const nlohmann::json& j) {
    // Written to a sibling then renamed: a launcher killed mid-write must not
    // leave a truncated cache that fails to parse on the next start.
    std::string tmp = path + ".tmp";
    { std::ofstream fo(tmp); if (!fo) return; fo << j.dump(); }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

std::string outbox_dir() { return store_path("hiscore-outbox"); }
std::string cache_file() { return store_path("hiscore-cache.json"); }

} // namespace

void set_store_dir(const std::string& dir) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_store_dir = dir; }
    std::error_code ec;
    std::filesystem::create_directories(outbox_dir(), ec);
}

void queue_submission(const std::string& system, const std::string& game,
                      const std::string& player, const std::string& country,
                      const Playtime& playtime,
                      const std::string& hi_before, const std::string& hi_after) {
    const std::string dir = outbox_dir();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    nlohmann::json j;
    j["system"] = system; j["game"] = game;
    j["player"] = player; j["country"] = country;
    j["play_last"] = playtime.last;
    j["play_longest"] = playtime.longest;
    j["play_total"] = playtime.total;
    j["hi_before"] = to_hex(hi_before);
    j["hi_after"]  = to_hex(hi_after);
    j["queued_at"] = now_iso8601();

    // The name carries the clock and a counter so two sessions finishing in
    // the same second cannot overwrite one another.
    static std::atomic<unsigned> seq{0};
    std::string name = std::to_string((long long)std::time(nullptr)) + "-" +
                       std::to_string(seq++) + ".json";
    write_json_file(dir + "/" + name, j);
}

int outbox_size() {
    const std::string dir = outbox_dir();
    if (dir.empty()) return 0;
    std::error_code ec;
    int n = 0;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".json") n++;
    return n;
}

int flush_outbox() {
    const std::string dir = outbox_dir();
    if (dir.empty() || base_url().empty()) return 0;

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());   // oldest first, so order is kept

    int sent = 0;
    for (const auto& f : files) {
        auto j = read_json_file(f.string());
        if (!j.is_object() || !j.contains("game")) {
            std::filesystem::remove(f, ec);      // unreadable: drop it
            continue;
        }
        Playtime pt{ j.value("play_last", 0), j.value("play_longest", 0),
                     j.value("play_total", 0) };
        auto r = submit(j.value("system", ""), j.value("game", ""),
                        j.value("player", ""), j.value("country", ""), pt,
                        from_hex(j.value("hi_before", "")),
                        from_hex(j.value("hi_after", "")));
        // Only a reply removes the entry. A transport failure means we are
        // still offline, and there is no point walking the rest of the queue.
        if (!r.reached) break;
        std::filesystem::remove(f, ec);
        sent++;
    }
    return sent;
}

void cache_supported(const std::set<std::string>& supported) {
    if (cache_file().empty()) return;
    auto j = read_json_file(cache_file());
    nlohmann::json list = nlohmann::json::array();
    for (const auto& k : supported) list.push_back(k);
    j["supported"] = list;
    j["supported_at"] = now_iso8601();
    write_json_file(cache_file(), j);
}

std::set<std::string> cached_supported() {
    std::set<std::string> out;
    if (cache_file().empty()) return out;
    auto j = read_json_file(cache_file());
    if (!j.contains("supported") || !j["supported"].is_array()) return out;
    for (const auto& v : j["supported"])
        if (v.is_string()) out.insert(v.get<std::string>());
    return out;
}

void cache_top(const std::string& system, const std::string& game,
               const std::vector<Entry>& rows) {
    if (cache_file().empty()) return;
    auto j = read_json_file(cache_file());
    nlohmann::json entry;
    entry["at"] = now_iso8601();
    nlohmann::json list = nlohmann::json::array();
    for (const auto& e : rows)
        list.push_back({{"player", e.player}, {"country", e.country},
                        {"score", e.score}, {"since", e.since}});
    entry["rows"] = list;
    j["tops"][key(system, game)] = entry;
    write_json_file(cache_file(), j);
}

void cache_boards(const std::vector<Board>& boards) {
    if (cache_file().empty()) return;
    auto j = read_json_file(cache_file());
    const std::string at = now_iso8601();
    for (const auto& b : boards) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& e : b.rows)
            list.push_back({{"player", e.player}, {"country", e.country},
                            {"score", e.score}, {"since", e.since}});
        j["tops"][key(b.system, b.game)] = {{"at", at}, {"rows", list}};
    }
    j["boards_at"] = at;
    write_json_file(cache_file(), j);
}

std::vector<Entry> cached_top(const std::string& system, const std::string& game,
                              std::string* fetched_at) {
    std::vector<Entry> out;
    if (fetched_at) fetched_at->clear();
    if (cache_file().empty()) return out;
    auto j = read_json_file(cache_file());
    if (!j.contains("tops")) return out;
    const std::string k = key(system, game);
    if (!j["tops"].contains(k)) return out;
    const auto& entry = j["tops"][k];
    if (fetched_at) *fetched_at = entry.value("at", "");
    if (!entry.contains("rows") || !entry["rows"].is_array()) return out;
    for (const auto& v : entry["rows"]) {
        Entry e;
        e.player  = v.value("player", "");
        e.country = v.value("country", "");
        e.since   = v.value("since", "");
        if (v.contains("score") && v["score"].is_number())
            e.score = v["score"].get<long long>();
        if (!e.player.empty()) out.push_back(e);
    }
    return out;
}

} // namespace HiscoreClient
