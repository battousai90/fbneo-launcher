// src/HiscoreClient.h
//
// Read-only client for the Bootcade score service. Submitting a score is a
// separate concern and lands later; everything here is safe to call without
// the player having any account, pseudonym or opinion on the matter.
//
// Every call is blocking and MUST run off the GTK thread. The service lives
// on the network, so a slow or absent one would otherwise freeze the game
// list : and an unreachable service is the normal case for anyone running
// the launcher without the homelab.
#pragma once
#include <set>
#include <string>
#include <vector>

namespace HiscoreClient {

// Base URL of the service, e.g. "http://192.168.1.21". Read from
// config.json ("hiscore_url"); an empty value disables every call, which is
// what a fresh install with no server configured should do.
std::string base_url();
void        set_base_url(const std::string& url);

// "<system>\n<game>" keys for the games the service can rank. Sent as one
// list rather than asked per game: the alternative is 29 000 requests to
// paint one column.
std::set<std::string> fetch_supported();

struct Entry {
    std::string player;
    std::string country;   // ISO code, may be empty
    long long   score = 0;
    std::string since;     // ISO-8601 UTC, when the score was validated
};

// Every leaderboard in one call. Fetched once at startup rather than one
// request per game the player clicks: browsing twenty games used to mean
// twenty requests, each able to stall for the connect timeout.
struct Board {
    std::string system, game;
    std::vector<Entry> rows;
};
std::vector<Board> fetch_boards(int limit = 10);

// Writes the whole set in one pass. Calling cache_top() in a loop would
// re-read and rewrite the cache file for each of the three hundred boards.
void cache_boards(const std::vector<Board>& boards);

// Leaderboard for one game, best first. Empty on any failure : a missing
// leaderboard and an unreachable server look the same to the player, and
// neither is worth an error dialog over a game they were merely browsing.
std::vector<Entry> fetch_top(const std::string& system,
                             const std::string& game,
                             int limit = 50);

// Outcome of a submission. `reached` distinguishes "the service answered"
// from "we never got there": an unreachable server is not the player's
// problem and must not be reported to them as a rejected score.
struct SubmitResult {
    bool        reached = false;
    bool        accepted = false;   // published straight away
    bool        pending  = false;   // queued for an administrator
    // The service looked and found nothing attributable : the table did not
    // move, the game has no known encoding. Not a failure and not the
    // player's problem: nothing to say.
    bool        ignored  = false;
    long long   score = 0;
    bool        has_score = false;
    std::string reason;             // why it was queued, or why it was refused
    std::string error;              // transport-level failure
};

// How long this player has spent on this game, sent alongside the score under
// the same consent. Zeroes mean "do not report".
struct Playtime {
    int last = 0, longest = 0, total = 0;
};

// Send one session. `hi_before` may be empty : a first-ever game on a title
// has no baseline : and the service will queue rather than publish it.
SubmitResult submit(const std::string& system,
                    const std::string& game,
                    const std::string& player,
                    const std::string& country,   // ISO code, may be empty
                    const Playtime&    playtime,
                    const std::string& hi_before,
                    const std::string& hi_after);

// ── Offline store ─────────────────────────────────────────────────────────
// Kept in the launcher's own config directory rather than in games.db: that
// database is rebuilt from the DAT files whenever the library is rescanned,
// and a queued score must not be collateral damage of a ROM scan.
void set_store_dir(const std::string& dir);

// Park a submission the network refused to take. Retried at the next start.
//
// This is the part that matters most: without it a score played offline is
// gone for good : the send is attempted once, and FBNeo overwrites the .hi on
// the next session. The player did the work and nothing recorded it.
void queue_submission(const std::string& system, const std::string& game,
                      const std::string& player, const std::string& country,
                      const Playtime& playtime,
                      const std::string& hi_before, const std::string& hi_after);

// Retry everything parked. Returns how many finally went through. Blocking.
int flush_outbox();
int outbox_size();

// Last known answers, so an offline launcher shows what it knew rather than
// nothing at all : with the date, because pretending stale data is fresh is
// worse than admitting it is old.
void cache_supported(const std::set<std::string>& supported);
std::set<std::string> cached_supported();

// Jeux dont la table se lit dans la SRAM de cartouche et non dans le .hi.
// Le service le dit dans /api/supported : le lanceur n'a pas les définitions
// hi2txt sous la main pour en décider seul, et se tromper de fichier publie
// un score qui n'est pas celui du joueur.
bool is_saveram(const std::string& system, const std::string& game);

// Met a jour le hiscore.dat de l'emulateur depuis le service.
//
// Celui livre avec FinalBurn Neo ignore une trentaine de jeux dont le service
// sait decoder les scores : pour ceux-la l'emulateur n'ecrit aucun fichier, et
// la pastille Highscore promet un classement que rien ne peut alimenter. Le
// joueur joue, et il ne se passe rien.
//
// Appele seulement quand les classements en ligne sont actives : sans cela le
// lanceur ne contacte jamais le service, ce qui est la regle de toute la
// fonctionnalite. Renvoie true si le fichier a ete remplace.
bool sync_hiscore_dat(const std::string& fbneo_hiscores_dir);

// Ce que mesure la table de ce jeu : "score", "time" ou "par". Tous ne
// rangent pas des points. Art of Fighting 3 range un chrono, affiche tel quel
// il donnerait 917504 au lieu de 14'00"00 ; Neo Turf Masters range un ecart au
// par, ou moins trois bat zero. Le service le dit dans /api/supported.
std::string metric_of(const std::string& system, const std::string& game);

void cache_top(const std::string& system, const std::string& game,
               const std::vector<Entry>& rows);
// `fetched_at` receives the ISO-8601 date the cache was written, or stays
// empty when nothing is cached.
/* Tout le cache d'un coup.
 *
 * Appeler cached_top pour chaque jeu relisait et reanalysait le fichier a
 * chaque appel : sur 29 000 jeux, c'est 29 000 lectures du meme fichier.
 * Quand on cherche a travers TOUS les classements, une seule lecture suffit.
 */
std::vector<Board> cached_all_boards();

std::vector<Entry> cached_top(const std::string& system, const std::string& game,
                              std::string* fetched_at);

// Key used in the supported-games set.
inline std::string key(const std::string& system, const std::string& game) {
    return system + "\n" + game;
}

} // namespace HiscoreClient
