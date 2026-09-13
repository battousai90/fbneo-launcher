// src/DatSource.h
//
// DAT groups, DAT sources, and how DAT files are kept current.
//
// A DAT GROUP is a named selection of DAT files the user organises as they
// please : "FinalBurn Neo" (every DAT), "FBNeo - GBA" (one file), "Special
// Arcade" (a hand-picked few). It is not an emulator and not a source. The
// Library audits against one group at a time, so a group is also how a
// player narrows what "complete" means for them.
//
// A DAT SOURCE is where files come from : the emulator itself (fbneo -dat),
// an HTTP location publishing a dat-manifest.json, or a folder filled by
// hand. The files of a source live in one folder on disk. Several groups may
// share the same source and folder : each keeps its own selection, the
// files are stored once, and the database loads the union of what active
// groups select, each file once.
//
// Groups live in config.json (rom_manager.dat_groups) : they are names,
// paths and URLs, like every other setting. The database keeps only what
// it derives from the files.
//
// The HTTP contract (dat-manifest.json, schema_version 1) is published by
// the file server next to the files it lists. The SHA256 is the only thing
// this code trusts to say "changed"; version and date are for the eye. A
// download lands in a temporary file, is checked for size and SHA256, and
// only then takes the DAT's place, atomically. A mismatch leaves the local
// DAT untouched and the temporary file gone.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace DatSource {

enum class Kind { Emulator, Http, Folder };
const char* kind_key(Kind k);          // "emulator" | "http" | "folder"
Kind        kind_from_key(const std::string& s);

constexpr int kManifestSchema = 1;
// The Bootcade file server : what a fresh install points at.
constexpr const char* kDefaultManifestUrl = "https://files.gcourtot.duckdns.org/dat/dat-manifest.json";

struct Group {
    std::string id;                 // "fbneo", "fbneo-gba", … : stable, never shown
    std::string name;               // "FinalBurn Neo", "FBNeo - GBA"
    std::string folder;             // where the source's .dat files live
    Kind        source = Kind::Http;
    std::string url;                // manifest URL, Kind::Http
    std::string set_style = "non-merged";   // how the library this group describes is laid out
    bool        active = true;      // an inactive group loads nothing and is not offered for audit
    // The selection. all_files: every DAT file the source provides, today
    // and tomorrow. Otherwise `files` names them one by one.
    bool        all_files = true;
    std::vector<std::string> files;
    std::string last_check;         // ISO-8601, Kind::Http
    std::string last_update;        // ISO-8601, any source: last time the folder changed through us

    // Is this DAT file part of the group?
    bool selects(const std::string& file) const;
    // Include or exclude one file. Leaving all_files for an explicit list
    // needs the list of what the source provides right now.
    void set_selected(const std::string& file, bool on, const std::vector<std::string>& provided);
};

// config.json ⇄ groups. A config with no dat_groups yet gets one group built
// from the legacy keys (dat_path), so nothing changes for an existing user
// until they touch the screen. The previous shape of a group (inactive_files)
// is migrated into a selection.
std::vector<Group> load_groups();
bool               save_groups(const std::vector<Group>& groups);
// A new id from a name, unique among `taken`.
std::string        make_id(const std::string& name, const std::vector<Group>& taken);

// The DAT files present in a folder (*.dat, sorted by name).
std::vector<std::string> list_folder(const std::string& folder);

// Every DAT file the database must be built from : the union of what active
// groups select, one path per file name. Two groups pointing at different
// folders for the same file name would collide in the database (one row per
// game and system) : the first group wins and `conflicts` says so.
std::vector<std::string> files_to_load(const std::vector<Group>& groups,
                                       std::vector<std::string>* conflicts = nullptr);
// The file names one group selects among what its folder holds.
std::set<std::string> selected_in_folder(const Group& g);

// ── HTTP contract ───────────────────────────────────────────────────────────

struct RemoteFile {
    std::string name;
    uint64_t    size = 0;
    std::string sha256;
    std::string version;
    std::string date;
};

struct Manifest {
    int         schema_version = 0;
    std::string generated, group;
    std::vector<RemoteFile> files;
};

// Fetches and parses a manifest. Refuses any schema this build does not
// know : `error` then says which one it met. Network work : call off the
// GTK thread.
bool fetch_manifest(const std::string& url, Manifest& out, std::string& error);

// Local file against the manifest. Sha256 of local files is computed here.
enum class State { UpToDate, Outdated, Missing, LocalOnly };
struct FileStatus {
    std::string name;
    State       state = State::LocalOnly;
    uint64_t    local_size = 0, remote_size = 0;
    std::string local_sha256, remote_sha256;
    std::string remote_version, remote_date;
};
std::vector<FileStatus> compare(const std::string& folder, const Manifest& manifest);

// Downloads one file of the manifest into `folder`, through a temporary
// name; size and SHA256 are checked before the rename. On any failure the
// current local file (if any) is untouched and the temporary file removed.
using Progress = std::function<void(double pct, const std::string& message)>;
bool download(const std::string& manifest_url, const RemoteFile& file,
              const std::string& folder, std::string& error,
              const Progress& progress = nullptr,
              const std::function<bool()>& cancelled = nullptr);

// SHA256 of a file, lower-case hex. Empty on error.
std::string sha256_of(const std::string& path);

// The header of a DAT file, without parsing its games.
struct Header {
    std::string name, description, version, date, author;
    std::vector<std::string> preview;   // first lines of the file, for the eye
};
Header read_header(const std::string& path, int preview_lines = 14);

std::string now_iso();

} // namespace DatSource
