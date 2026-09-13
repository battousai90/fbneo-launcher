// src/RomResolve.cpp
#include "RomResolve.h"

#include "AppContext.h"
#include "RomScanner.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace RomResolve {
namespace {

unsigned long parse_crc_hex(const std::string& hex) {
    if (hex.empty()) return 0;
    unsigned long crc = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> crc;
    return crc;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// The folder a set's archive is expected in : its raw DAT header, or the same
// reconstruction RomAudit and the outbox use when the header was never stored.
std::string expected_folder(const Game& g) {
    return g.dat_header.empty() ? ("FinalBurn Neo - " + g.system + " Games") : g.dat_header;
}

// One archive, one ROM: the scanner's historical rule, unchanged. Name first
// (raw, then normalized), CRC as the fallback that turns a stray name into
// "wrong name" rather than "absent".
struct Probe {
    RomState      state = RomState::Absent;
    std::string   entry;   // the entry that matched, when one did
    unsigned long crc = 0; // its CRC (the wrong one, for Corrupt)
};

Probe probe(const Archive* a, const std::string& wanted_name, unsigned long crc) {
    Probe p;
    if (!a) return p;
    auto it = a->crc_by_name.find(wanted_name);
    if (it == a->crc_by_name.end())
        it = a->crc_by_name.find(RomScanner::normalize_name(wanted_name));
    bool name_present = (it != a->crc_by_name.end());
    if (name_present && it->second == crc) {
        p.state = RomState::Present;
        p.entry = wanted_name;
        p.crc   = crc;
        return p;
    }
    auto by_crc = a->name_by_crc.find(crc);
    if (by_crc != a->name_by_crc.end()) {
        p.state = RomState::WrongName;
        p.entry = by_crc->second;
        p.crc   = crc;
        return p;
    }
    p.state = name_present ? RomState::Corrupt : RomState::Absent;
    if (name_present) { p.entry = wanted_name; p.crc = it->second; }
    return p;
}

bool has_data(RomState s) { return s == RomState::Present || s == RomState::WrongName; }

} // namespace

// ── Style ───────────────────────────────────────────────────────────────────

SetStyle style_from_string(const std::string& s) {
    std::string v = lower(s);
    if (v == "split") return SetStyle::Split;
    return SetStyle::NonMerged;
}

std::string to_string(SetStyle s) {
    switch (s) {
        case SetStyle::Split: return "split";
        default:              return "non-merged";
    }
}

SetStyle load_style() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { return SetStyle::NonMerged; } }
    if (!j.contains("rom_manager") || !j["rom_manager"].is_object()) return SetStyle::NonMerged;
    const auto& rm = j["rom_manager"];
    std::string wanted = (rm.contains("library_group") && rm["library_group"].is_string()) ? rm["library_group"].get<std::string>() : "";
    if (rm.contains("dat_groups") && rm["dat_groups"].is_array()) {
        std::string first_style, chosen_style;
        for (const auto& g : rm["dat_groups"]) {
            if (!g.is_object()) continue;
            std::string style = (g.contains("set_style") && g["set_style"].is_string()) ? g["set_style"].get<std::string>() : "";
            if (first_style.empty()) first_style = style;
            if (!wanted.empty() && g.contains("id") && g["id"].is_string() && g["id"].get<std::string>() == wanted) chosen_style = style;
        }
        if (!chosen_style.empty()) return style_from_string(chosen_style);
        if (!first_style.empty())  return style_from_string(first_style);
    }
    // Before groups existed the style was one key for the whole library.
    if (rm.contains("set_style") && rm["set_style"].is_string())
        return style_from_string(rm["set_style"].get<std::string>());
    return SetStyle::NonMerged;
}

// ── Archive ─────────────────────────────────────────────────────────────────

void Archive::add(const std::string& entry_name, unsigned long crc) {
    crc_by_name[entry_name] = crc;
    crc_by_name[RomScanner::normalize_name(entry_name)] = crc;
    name_by_crc.emplace(crc, entry_name);
    entries.push_back(entry_name);
}

// ── The rule ────────────────────────────────────────────────────────────────

Verdict evaluate(const Game& game, const Archive* own, SetStyle style,
                 const ArchiveLookup& archive_for, const GameLookup& game_for) {
    Verdict v;
    if (game.roms.empty()) return v;

    bool all_present = true, all_correct = true;
    for (const auto& rom : game.roms) {
        if (rom.crc.empty()) continue;   // nodump: nothing to verify against

        RomVerdict r;
        r.name      = rom.name;
        r.crc       = parse_crc_hex(rom.crc);
        r.size      = (uint64_t)rom.size;
        r.inherited = rom.is_inherited();

        // The set's own archive is always consulted first, whatever the style:
        // a split collection may still carry a copy of an inherited ROM, and
        // FBNeo is happy either way.
        Probe p = probe(own, rom.name, r.crc);
        r.state = p.state;
        r.found_crc = p.crc;
        if (has_data(p.state) && p.entry != rom.name) r.found_as = p.entry;

        // Split: an inherited ROM the set's own zip cannot vouch for is looked
        // for up the romof chain (parent, then the parent's parent, then the
        // BIOS), under the name the DAT says it carries there. A ROM the DAT
        // marks as the set's own never takes this path: "it exists in some
        // other zip" is precisely what must not count as present.
        if (style == SetStyle::Split && r.inherited && !has_data(r.state) && archive_for && game_for) {
            std::string name = game.romof, system = game.system;
            for (int depth = 0; depth < 8 && !name.empty(); ++depth) {
                Game ancestor = game_for(name, system);
                if (ancestor.name.empty()) break;
                Probe q = probe(archive_for(ancestor), rom.merge, r.crc);
                if (has_data(q.state)) {
                    r.state          = q.state;
                    r.found_crc      = q.crc;
                    r.inherited_from = ancestor.name;
                    const Archive* a = archive_for(ancestor);
                    if (a) r.found_in = a->path;
                    if (q.entry != rom.name) r.found_as = q.entry;
                    break;
                }
                if (ancestor.romof == name) break;   // a set naming itself as parent
                name = ancestor.romof;
            }
        }

        if (r.state == RomState::Absent)                                    all_present = false;
        else if (r.state == RomState::WrongName || r.state == RomState::Corrupt) all_correct = false;
        v.roms.push_back(std::move(r));
    }

    if (v.roms.empty()) return v;
    if (!all_present)      v.status = "missing";
    else if (!all_correct) v.status = "incorrect";
    else                   v.status = "available";
    return v;
}

std::string status_of(const Game& game, const Archive* own, SetStyle style,
                      const ArchiveLookup& archive_for, const GameLookup& game_for) {
    return evaluate(game, own, style, archive_for, game_for).status;
}

// ── CacheIndex ──────────────────────────────────────────────────────────────

CacheIndex::CacheIndex(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roots_in) {
    std::error_code ec;
    std::vector<DatabaseManager::ZipContentRow> rows;
    db->getAllZipContents(rows);

    std::vector<fs::path> roots;
    for (const auto& r : roots_in)
        if (!r.empty()) roots.push_back(fs::weakly_canonical(fs::path(r), ec));

    auto under_roots = [&](const fs::path& p) {
        if (roots.empty()) return true;
        for (const auto& root : roots) {
            auto it_r = root.begin(), end_r = root.end();
            auto it_p = p.begin(), end_p = p.end();
            bool ok = true;
            for (; it_r != end_r; ++it_r, ++it_p) {
                if (it_p == end_p || *it_p != *it_r) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    };

    std::unordered_map<std::string, bool> usable;
    for (const auto& r : rows) {
        auto u = usable.find(r.filepath);
        if (u == usable.end()) {
            fs::path p = fs::weakly_canonical(fs::path(r.filepath), ec);
            bool ok = fs::exists(p, ec) && under_roots(p);
            u = usable.emplace(r.filepath, ok).first;
            if (ok) {
                m_by_stem[lower(fs::path(r.filepath).stem().string())].push_back(r.filepath);
                m_archives[r.filepath].path = r.filepath;
            }
        }
        if (!u->second) continue;
        m_archives[r.filepath].add(r.entry_name, r.crc);
    }
}

const Archive* CacheIndex::by_path(const std::string& path) const {
    auto it = m_archives.find(path);
    return it == m_archives.end() ? nullptr : &it->second;
}

const Archive* CacheIndex::for_game(const Game& game) const {
    auto cand = m_by_stem.find(lower(game.name));
    if (cand == m_by_stem.end() || cand->second.empty()) return nullptr;

    // Several same-named archives can coexist (same short name under two
    // systems; an outbox added to roms_paths mirroring the library's folder
    // names). Folder name alone cannot break the tie : a broken duplicate in an
    // outbox sits under the very same folder name as the good copy. Score each
    // candidate by how many of the set's ROMs it satisfies by name and CRC; the
    // folder name only settles a genuine tie.
    const std::string folder = expected_folder(game);
    const Archive* best = nullptr;
    int  best_score = -1;
    bool best_dir_match = false;
    for (const auto& path : cand->second) {
        const Archive* a = by_path(path);
        if (!a) continue;
        int score = 0;
        for (const auto& rom : game.roms) {
            if (rom.crc.empty()) continue;
            unsigned long want = parse_crc_hex(rom.crc);
            auto it = a->crc_by_name.find(rom.name);
            if (it == a->crc_by_name.end())
                it = a->crc_by_name.find(RomScanner::normalize_name(rom.name));
            if (it != a->crc_by_name.end() && it->second == want) ++score;
        }
        bool dir_match = fs::path(path).parent_path().filename().string() == folder;
        if (score > best_score || (score == best_score && dir_match && !best_dir_match)) {
            best = a;
            best_score = score;
            best_dir_match = dir_match;
        }
    }
    return best;
}

// ── Cache pass for inherited ROMs ───────────────────────────────────────────

int resolve_inherited_from_cache(std::shared_ptr<DatabaseManager> db,
                                 const std::vector<std::string>& roots,
                                 SetStyle style,
                                 const std::unordered_set<std::string>& touched) {
    if (style != SetStyle::Split) return 0;

    CacheIndex index(db, roots);
    if (index.empty()) return 0;

    std::vector<Game> games = db->getAllGames();
    std::unordered_map<std::string, size_t> by_key;
    by_key.reserve(games.size());
    for (size_t i = 0; i < games.size(); ++i)
        by_key[games[i].name + '\x1f' + games[i].system] = i;

    GameLookup game_for = [&](const std::string& name, const std::string& system) -> Game {
        auto it = by_key.find(name + '\x1f' + system);
        return it == by_key.end() ? Game{} : games[it->second];
    };
    ArchiveLookup archive_for = [&](const Game& g) { return index.for_game(g); };

    // "Affected by what was just scanned": the set itself, or any of its
    // ancestors. Walked through the in-memory list, never the database.
    auto is_touched = [&](const Game& g) {
        if (touched.empty()) return true;
        if (touched.count(lower(g.name))) return true;
        std::string name = g.romof;
        for (int depth = 0; depth < 8 && !name.empty(); ++depth) {
            if (touched.count(lower(name))) return true;
            auto it = by_key.find(name + '\x1f' + g.system);
            if (it == by_key.end()) break;
            const std::string& next = games[it->second].romof;
            if (next == name) break;
            name = next;
        }
        return false;
    };

    int changed = 0;
    db->beginTransaction();
    for (const auto& g : games) {
        bool inherits = std::any_of(g.roms.begin(), g.roms.end(),
                                    [](const Rom& r) { return r.is_inherited(); });
        if (!inherits || !is_touched(g)) continue;

        const Archive* own = index.for_game(g);
        if (!own) continue;   // the cache has nothing to say about this set

        std::string status = status_of(g, own, style, archive_for, game_for);
        if (status.empty() || status == g.status) continue;
        // With the folder the set's own archive sits in, like the live scan
        // records it : that is what lets a removed ROM directory take its
        // sets' statuses away with it.
        db->updateGameStatusWithSource(g.name, status, g.system,
                                       fs::path(own->path).parent_path().string());
        ++changed;
    }
    db->commitTransaction();
    return changed;
}

} // namespace RomResolve
