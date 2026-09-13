// src/DatSource.cpp
#include "DatSource.h"

#include "AppContext.h"

#include <curl/curl.h>
#include <glibmm/checksum.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

namespace DatSource {
namespace {

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_to_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, (std::streamsize)(size * nmemb));
    return out->good() ? size * nmemb : 0;
}

struct DownloadCtx {
    const Progress* progress;
    const std::function<bool()>* cancelled;
    std::string label;
};

int progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadCtx*>(clientp);
    if (ctx->cancelled && *ctx->cancelled && (*ctx->cancelled)()) return 1;   // aborts the transfer
    if (ctx->progress && *ctx->progress && dltotal > 0)
        (*ctx->progress)(100.0 * (double)dlnow / (double)dltotal, ctx->label);
    return 0;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// "…/dat/dat-manifest.json" + "FBNeo_-_Arcade.dat" → "…/dat/FBNeo_-_Arcade.dat".
// The files sit next to the manifest : that is the contract.
std::string sibling_url(const std::string& manifest_url, const std::string& name) {
    std::string base = manifest_url;
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(0, slash + 1);
    // Percent-encode what a DAT name can carry that a URL cannot.
    std::string enc;
    for (unsigned char c : name) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') enc += (char)c;
        else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); enc += buf; }
    }
    return base + enc;
}

} // namespace

// ── Model ───────────────────────────────────────────────────────────────────

const char* kind_key(Kind k) {
    switch (k) {
        case Kind::Emulator: return "emulator";
        case Kind::Folder:   return "folder";
        default:             return "http";
    }
}

Kind kind_from_key(const std::string& s) {
    std::string v = lower(s);
    if (v == "emulator") return Kind::Emulator;
    if (v == "folder")   return Kind::Folder;
    return Kind::Http;
}

bool Group::selects(const std::string& file) const {
    if (all_files) return true;
    return std::find(files.begin(), files.end(), file) != files.end();
}

void Group::set_selected(const std::string& file, bool on, const std::vector<std::string>& provided) {
    if (all_files) {
        if (on) return;
        // Leaving "everything" : the list starts as everything the source
        // provides right now, minus this one.
        all_files = false;
        files = provided;
    }
    files.erase(std::remove(files.begin(), files.end(), file), files.end());
    if (on) files.push_back(file);
    std::sort(files.begin(), files.end());
    // Back to "everything" when the list covers the whole source again : a
    // file appearing later is then taken in, as the user meant.
    if (!provided.empty()) {
        bool all = true;
        for (const auto& p : provided) if (std::find(files.begin(), files.end(), p) == files.end()) { all = false; break; }
        if (all) { all_files = true; files.clear(); }
    }
}

std::string now_iso() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::vector<std::string> list_folder(const std::string& folder) {
    std::vector<std::string> out;
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return out;
    for (auto it = fs::directory_iterator(folder, ec); it != fs::directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        if (lower(it->path().extension().string()) != ".dat") continue;
        out.push_back(it->path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string make_id(const std::string& name, const std::vector<Group>& taken) {
    std::string base;
    for (unsigned char c : lower(name)) {
        if (isalnum(c)) base += (char)c;
        else if (!base.empty() && base.back() != '-') base += '-';
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "group";
    auto used = [&](const std::string& id) { for (const auto& g : taken) if (g.id == id) return true; return false; };
    std::string id = base;
    for (int n = 2; used(id); ++n) id = base + "-" + std::to_string(n);
    return id;
}

std::vector<Group> load_groups() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }

    std::vector<Group> groups;
    auto str = [](const nlohmann::json& o, const char* k, const std::string& d = "") {
        return (o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : d;
    };
    auto flag = [](const nlohmann::json& o, const char* k, bool d) {
        return (o.contains(k) && o[k].is_boolean()) ? o[k].get<bool>() : d;
    };
    auto list = [](const nlohmann::json& o, const char* k) {
        std::vector<std::string> v;
        if (o.contains(k) && o[k].is_array())
            for (const auto& f : o[k]) if (f.is_string()) v.push_back(f.get<std::string>());
        return v;
    };
    const std::string legacy_style = (j.contains("rom_manager") && j["rom_manager"].is_object())
                                   ? str(j["rom_manager"], "set_style", "non-merged") : "non-merged";
    if (j.contains("rom_manager") && j["rom_manager"].is_object()
        && j["rom_manager"].contains("dat_groups") && j["rom_manager"]["dat_groups"].is_array()) {
        for (const auto& g : j["rom_manager"]["dat_groups"]) {
            if (!g.is_object()) continue;
            Group grp;
            grp.id          = str(g, "id");
            grp.name        = str(g, "name");
            grp.folder      = str(g, "folder");
            grp.source      = kind_from_key(str(g, "source", "http"));
            grp.url         = str(g, "url", kDefaultManifestUrl);
            grp.set_style   = str(g, "set_style", legacy_style);
            grp.active      = flag(g, "active", true);
            grp.last_check  = str(g, "last_check");
            grp.last_update = str(g, "last_update");
            if (g.contains("all_files") || g.contains("files")) {
                grp.all_files = flag(g, "all_files", true);
                grp.files     = list(g, "files");
            } else {
                // Previous shape : every file of the folder, minus inactive
                // ones. Same meaning, expressed as a selection.
                std::vector<std::string> inactive = list(g, "inactive_files");
                if (inactive.empty()) {
                    grp.all_files = true;
                } else {
                    grp.all_files = false;
                    for (const auto& f : list_folder(grp.folder))
                        if (std::find(inactive.begin(), inactive.end(), f) == inactive.end()) grp.files.push_back(f);
                }
            }
            if (!grp.id.empty()) groups.push_back(std::move(grp));
        }
    }
    if (groups.empty()) {
        // First run of this model : the one group every install has, built
        // from the folder the launcher already used. Its source is the
        // Bootcade file server : the way DATs reach a user who never runs
        // fbneo -dat by hand. Not saved here : nothing changes on disk until
        // the user acts on the screen.
        Group fb;
        fb.id        = "fbneo";
        fb.name      = "FinalBurn Neo";
        fb.folder    = str(j, "dat_path");
        fb.source    = Kind::Http;
        fb.url       = kDefaultManifestUrl;
        fb.set_style = legacy_style;
        groups.push_back(std::move(fb));
    }
    return groups;
}

bool save_groups(const std::vector<Group>& groups) {
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
    auto arr = nlohmann::json::array();
    for (const auto& g : groups) {
        nlohmann::json o;
        o["id"] = g.id;
        o["name"] = g.name;
        o["folder"] = g.folder;
        o["source"] = kind_key(g.source);
        o["url"] = g.url;
        o["set_style"] = g.set_style;
        o["active"] = g.active;
        o["all_files"] = g.all_files;
        o["files"] = g.files;
        o["last_check"] = g.last_check;
        o["last_update"] = g.last_update;
        arr.push_back(std::move(o));
    }
    j["rom_manager"]["dat_groups"] = std::move(arr);
    // The first group's folder is also the launcher's dat_path : the two keys
    // name the same thing, and the rest of the application reads the old one.
    if (!groups.empty()) j["dat_path"] = groups.front().folder;
    std::ofstream fo(path);
    if (!fo) return false;
    fo << j.dump(4);
    return true;
}

std::set<std::string> selected_in_folder(const Group& g) {
    std::set<std::string> out;
    for (const auto& f : list_folder(g.folder))
        if (g.selects(f)) out.insert(f);
    return out;
}

std::vector<std::string> files_to_load(const std::vector<Group>& groups, std::vector<std::string>* conflicts) {
    std::vector<std::string> out;
    std::map<std::string, std::string> seen;   // file name → folder it was taken from
    for (const auto& g : groups) {
        if (!g.active) continue;
        for (const auto& f : selected_in_folder(g)) {
            auto it = seen.find(f);
            if (it == seen.end()) {
                seen[f] = g.folder;
                out.push_back((fs::path(g.folder) / f).string());
            } else if (it->second != g.folder && conflicts) {
                conflicts->push_back(f + " (" + g.name + " : " + g.folder + " ignored, already loaded from " + it->second + ")");
            }
        }
    }
    return out;
}

// ── HTTP ────────────────────────────────────────────────────────────────────

bool fetch_manifest(const std::string& url, Manifest& out, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl_easy_init failed"; return false; }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bootcade");
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { error = curl_easy_strerror(res); return false; }
    if (status != 200) { error = "HTTP " + std::to_string(status); return false; }

    nlohmann::json j;
    try { j = nlohmann::json::parse(body); }
    catch (const std::exception& e) { error = std::string("not a JSON manifest: ") + e.what(); return false; }
    if (!j.is_object() || !j.contains("schema_version") || !j["schema_version"].is_number_integer()) {
        error = "not a dat-manifest.json (no schema_version)";
        return false;
    }
    out = Manifest{};
    out.schema_version = j["schema_version"].get<int>();
    if (out.schema_version != kManifestSchema) {
        error = "manifest schema " + std::to_string(out.schema_version) + " is not supported by this version of Bootcade (expected "
              + std::to_string(kManifestSchema) + ")";
        return false;
    }
    out.generated = j.value("generated", "");
    out.group     = j.value("group", "");
    if (j.contains("files") && j["files"].is_array()) {
        for (const auto& f : j["files"]) {
            if (!f.is_object()) continue;
            RemoteFile rf;
            rf.name    = f.value("name", "");
            rf.size    = f.contains("size") && f["size"].is_number() ? f["size"].get<uint64_t>() : 0;
            rf.sha256  = lower(f.value("sha256", ""));
            rf.version = f.value("version", "");
            rf.date    = f.value("date", "");
            // A name that could leave the folder is not a file we want.
            if (rf.name.empty() || rf.name.find('/') != std::string::npos || rf.name.find("..") != std::string::npos) continue;
            out.files.push_back(std::move(rf));
        }
    }
    return true;
}

std::string sha256_of(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    Glib::Checksum sum(Glib::Checksum::CHECKSUM_SHA256);
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0) sum.update(reinterpret_cast<const guchar*>(buf.data()), (gsize)n);
    }
    return lower(sum.get_string());
}

std::vector<FileStatus> compare(const std::string& folder, const Manifest& manifest) {
    std::vector<FileStatus> out;
    std::error_code ec;
    std::map<std::string, const RemoteFile*> remote;
    for (const auto& f : manifest.files) remote[f.name] = &f;

    for (const auto& [name, rf] : remote) {
        FileStatus st;
        st.name = name;
        st.remote_size = rf->size;
        st.remote_sha256 = rf->sha256;
        st.remote_version = rf->version;
        st.remote_date = rf->date;
        fs::path local = fs::path(folder) / name;
        if (!fs::exists(local, ec)) {
            st.state = State::Missing;
        } else {
            st.local_size = fs::file_size(local, ec);
            st.local_sha256 = sha256_of(local.string());
            st.state = (st.local_sha256 == rf->sha256) ? State::UpToDate : State::Outdated;
        }
        out.push_back(std::move(st));
    }
    // Local files the manifest does not know : shown, never touched.
    for (auto it = fs::directory_iterator(folder, ec); it != fs::directory_iterator(); ++it) {
        if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".dat") continue;
        std::string name = it->path().filename().string();
        if (remote.count(name)) continue;
        FileStatus st;
        st.name = name;
        st.state = State::LocalOnly;
        st.local_size = fs::file_size(it->path(), ec);
        out.push_back(std::move(st));
    }
    return out;
}

bool download(const std::string& manifest_url, const RemoteFile& file,
              const std::string& folder, std::string& error,
              const Progress& progress, const std::function<bool()>& cancelled) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path final_path = fs::path(folder) / file.name;
    // Hidden and suffixed : nothing that reads *.dat can mistake it for a
    // DAT, whatever happens to this process mid-transfer.
    const fs::path tmp_path = fs::path(folder) / ("." + file.name + ".download");
    fs::remove(tmp_path, ec);

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write in " + folder; return false; }
        CURL* curl = curl_easy_init();
        if (!curl) { error = "curl_easy_init failed"; return false; }
        DownloadCtx ctx{&progress, &cancelled, file.name};
        std::string url = sibling_url(manifest_url, file.name);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bootcade");
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        out.close();
        if (res == CURLE_ABORTED_BY_CALLBACK) { error = "cancelled"; fs::remove(tmp_path, ec); return false; }
        if (res != CURLE_OK) { error = curl_easy_strerror(res); fs::remove(tmp_path, ec); return false; }
        if (status != 200) { error = "HTTP " + std::to_string(status); fs::remove(tmp_path, ec); return false; }
    }

    // What the manifest promised, or nothing : the local DAT is not touched
    // by a download that does not match.
    uint64_t got = fs::file_size(tmp_path, ec);
    if (ec || got != file.size) {
        error = "size mismatch: got " + std::to_string(got) + " bytes, manifest says " + std::to_string(file.size);
        fs::remove(tmp_path, ec);
        return false;
    }
    std::string sum = sha256_of(tmp_path.string());
    if (sum != file.sha256) {
        error = "SHA256 mismatch (the file changed on the server, try again)";
        fs::remove(tmp_path, ec);
        return false;
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        error = "cannot replace " + final_path.string() + ": " + ec.message();
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

// ── Header ──────────────────────────────────────────────────────────────────

Header read_header(const std::string& path, int preview_lines) {
    Header h;
    std::ifstream in(path);
    std::string line;
    std::string head_text;
    int n = 0;
    // The <header> of a logiqx DAT sits in the first lines; reading them is
    // enough for both the preview and the fields, without loading megabytes
    // of <game> entries.
    while (std::getline(in, line) && n < 400) {
        if (n < preview_lines) h.preview.push_back(line);
        head_text += line + "\n";
        ++n;
        if (line.find("</header>") != std::string::npos) break;
    }
    size_t a = head_text.find("<header>"), b = head_text.find("</header>");
    if (a == std::string::npos || b == std::string::npos) return h;
    std::string xml = head_text.substr(a, b - a + 9);
    pugi::xml_document doc;
    if (!doc.load_string(xml.c_str())) return h;
    auto hd = doc.child("header");
    h.name        = hd.child("name").text().get();
    h.description = hd.child("description").text().get();
    h.version     = hd.child("version").text().get();
    h.date        = hd.child("date").text().get();
    h.author      = hd.child("author").text().get();
    return h;
}

} // namespace DatSource
