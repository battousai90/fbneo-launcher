// src/Game.h
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

/* Un champ multiple venu du DAT : « Platformer, Action ».
 *
 * La virgule separe les valeurs, et pas la barre oblique : les libelles de
 * FBNeo en contiennent deja (« Shooter / Horizontal / Sh'mup »), tandis
 * qu'aucun ne contient de virgule. Chaque valeur compte pour elle, sinon
 * filtrer sur « Action » ne sortirait que les jeux portant exactement la
 * meme combinaison.
 */
inline std::vector<std::string> split_dat_values(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        std::string piece = value.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t a = piece.find_first_not_of(" \t");
        size_t b = piece.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(piece.substr(a, b - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

inline bool value_list_contains(const std::string& list, const std::string& wanted) {
    for (const auto& one : split_dat_values(list)) {
        if (one == wanted) return true;
    }
    return false;
}

struct Rom {
    std::string name;
    size_t size;
    std::string crc;  // CRC32 en hexadécimal
};

struct Game {
    std::string name;
    std::string description;
    std::string year;
    std::string manufacturer;
    std::string system;  // System type extracted from DAT header
    std::vector<Rom> roms;
    std::string status = "missing";  // "available", "missing", "incorrect", "incomplete"
    
    // Video information
    std::string video_type = "";        // "raster", "vector", etc.
    std::string orientation = "";       // "horizontal", "vertical"
    std::string width = "";
    std::string height = "";
    std::string aspect_x = "";
    std::string aspect_y = "";
    
    // Driver information
    std::string driver_status = "";     // "good", "preliminary", "nodump"

    // Ce que le pilote FBNeo declare de lui-meme, ecrit dans le DAT par
    // notre fork (src/burner/dat.cpp). Absent des DAT d'amont : ces champs
    // restent vides, et les filtres correspondants ne s'affichent pas.
    //
    // `fb_hiscore` dit que FBNeo sait lire la table des scores de ce jeu.
    // C'est une question DIFFERENTE de « le service Bootcade classe-t-il ce
    // jeu », a laquelle repond HiscoreClient : 6 049 jeux contre 489.
    std::string genre = "";             // "Run 'n Gun", "Platformer, Action"
    std::string family = "";            // "Metal Slug", "Street Fighter"
    int         players = 0;            // 0 = non renseigne
    bool        fb_hiscore = false;
    
    // Additional information
    std::string comment = "";
    std::string cloneof = "";
    std::string romof = "";
    std::string sourcefile = "";
    std::string snapshot_path = "";  // Path to game screenshot/snapshot
    std::string dat_source = "";  // Source DAT file name
    // Raw <header><name> of the source DAT, e.g. "FinalBurn Neo - Arcade Games".
    // `system` is this string trimmed down to "Arcade"; the untrimmed form is what
    // the ROM manager names its output folders after, so a rebuilt tree drops
    // straight into a library laid out from the same DATs.
    std::string dat_header = "";

    // Play tracking & favourites
    bool        is_favorite     = false;
    std::string last_played     = "";   // ISO-8601 timestamp or empty
    int         play_count      = 0;
    int         play_time_secs  = 0;   // cumulative across every session
    // Kept alongside the cumulative total rather than derived from it: a sum
    // cannot answer "how long was my last go" or "what is my best run", and
    // recomputing either would need a per-session history we deliberately do
    // not keep : one row per game stays cheap on a 29 000-game catalogue.
    int         last_session_secs    = 0;
    int         longest_session_secs = 0;

    bool is_available() const { return status == "available"; }

    // ── Release-type classification ───────────────────────────────────────────
    // A game can carry several of these at once (a hack is usually also a clone,
    // a homebrew may be either), so these are independent predicates rather than
    // one enum. FBNeo DATs mark the nature of a set in its description, e.g.
    // "Airwolf (Hack, English)", "Jurl (48K) (HB)", "'99: The Last War (bootleg)";
    // parentage comes from the DAT's cloneof attribute.
    bool is_clone()     const { return !cloneof.empty(); }
    bool is_hack()      const { return desc_has("(hack"); }
    bool is_homebrew()  const { return desc_has("(hb)") || desc_has("(hb,") || desc_has("(hb "); }
    bool is_bootleg()   const { return desc_has("bootleg"); }
    bool is_prototype() const { return desc_has("(proto"); }

    // "Original" = an official commercial parent set. Homebrew is excluded even
    // though it is nobody's clone: it is an original work, but not an official
    // release, and lumping ~3.2k fan-made ZX Spectrum titles in with the
    // commercial catalogue is not what "original games only" is asked for.
    bool is_original() const {
        return !is_clone() && !is_hack() && !is_bootleg()
            && !is_prototype() && !is_homebrew();
    }

private:
    // Case-insensitive substring test against the description.
    bool desc_has(const std::string& needle) const {
        auto it = std::search(description.begin(), description.end(),
                              needle.begin(), needle.end(),
                              [](char a, char b) {
                                  return std::tolower((unsigned char)a) ==
                                         std::tolower((unsigned char)b);
                              });
        return it != description.end();
    }
};