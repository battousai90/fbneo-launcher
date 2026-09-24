// src/MameCatalog.h
//
// Le catalogue MAME n'est pas importe, il est interroge.
//
// `mame -listxml` sort 320 Mo et 371 752 <rom> : les recopier dans games.db
// reviendrait a entretenir une deuxieme verite, forcement en retard sur celle
// de l'emulateur installe. On ne garde donc que ce que l'interface affiche
// (nom, description, annee, fabricant, parent, fichier source, etat du pilote),
// soit 14 Mo lus en 2,5 s, et on redemande le reste a MAME quand un ecran en a
// besoin : `mame -listroms <machine>` repond en 45 ms.
//
// Le cache est invalide par l'attribut build= de MAME : une mise a jour de la
// distribution suffit a declencher une regeneration, et il n'y a aucun DAT a
// telecharger ni a tenir a jour.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Game.h"

class DatabaseManager;

namespace MameCatalog {

// Le chemin de l'executable MAME, ou une chaine vide s'il est introuvable.
// Cherche dans le PATH de l'hote : sous Flatpak, MAME vit en dehors du bac a
// sable, comme FinalBurn Neo.
std::string find_executable();

// La version annoncee par le binaire, p.ex. "0.289 (unknown)". Chaine vide si
// MAME ne repond pas. C'est la cle d'invalidation du cache : elle est gardee
// telle quelle, suffixe compris, pour qu'un meme MAME ne se mette jamais a
// ressembler a un autre et ne declenche pas de regeneration inutile.
std::string installed_build(const std::string& mame_exe);

// Le seul numero de version, "0.289" pour "0.289 (unknown)".
//
// Ce qui suit le numero est l'identifiant de la revision construite, que les
// paquets des distributions ne renseignent pas : MAME ecrit alors le mot
// « unknown », qui se lit a l'ecran comme une panne alors que tout va bien.
std::string version_number(const std::string& raw);

// La derniere version publiee par MAMEdev, lue sur la page des sorties.
//
// MAMEdev ne publie AUCUN binaire Linux, seulement les sources : on ne peut
// donc que signaler l'ecart et dire ou regarder, jamais proposer de
// telecharger quoi que ce soit comme on le fait pour FinalBurn Neo.
struct LatestRelease {
    bool ok = false;         // false des qu'on n'a pas su lire un numero
    std::string version;     // "0.290"
    std::string error;       // renseigne quand ok == false
};
LatestRelease fetch_latest_release();

// Compare deux numeros facon MAME ("0.289"). Rend <0, 0 ou >0. Un numero
// illisible rend 0 : on prefere ne rien affirmer a affirmer n'importe quoi.
int compare_versions(const std::string& a, const std::string& b);

// La version pour laquelle le cache a ete construit, telle qu'enregistree.
std::string cached_build(const std::shared_ptr<DatabaseManager>& db);

// Relit le catalogue depuis MAME et remplace la table. Rend le nombre de
// machines enregistrees, ou -1 en cas d'echec. `progress` est appele de temps
// en temps avec le compte courant, jamais depuis un autre fil que l'appelant.
int rebuild(const std::shared_ptr<DatabaseManager>& db,
            const std::string& mame_exe,
            const std::function<void(int)>& progress = {});

// Regenere seulement si la version installee differe de celle du cache.
// Rend le nombre de machines disponibles ensuite.
int sync(const std::shared_ptr<DatabaseManager>& db,
         const std::string& mame_exe,
         const std::function<void(int)>& progress = {});

// Les dossiers de ROMs declares dans ~/.mame/mame.ini, pour les proposer a
// l'utilisateur. Chaque entree est rendue telle quelle, y compris si elle
// n'existe plus : c'est justement ce qu'il faut lui montrer.
std::vector<std::string> rompaths_from_mame_ini();

// Verdict de MAME sur la collection, ecrit dans le catalogue.
//
// On ne reimplemente pas la regle : `mame -verifyroms` connait les sets
// splits, les BIOS et les peripheriques mieux que nous, et il ne peut pas
// etre en desaccord avec lui-meme. Sa sortie est simplement traduite dans le
// vocabulaire du launcher.
//
// Les sets absents ne produisent aucune ligne : ils se deduisent par
// difference avec le catalogue. Rend le nombre de sets trouves, ou -1.
struct AuditResult {
    int good = 0;        // "is good"
    int playable = 0;    // "is best available" : jouable, dumps manquants inconnus
    int bad = 0;         // "is bad" : present mais abime
    int missing = 0;     // jamais mentionne par MAME
};
AuditResult audit(const std::shared_ptr<DatabaseManager>& db,
                  const std::string& mame_exe,
                  const std::vector<std::string>& rompaths,
                  const std::function<bool(int)>& progress = {});

// Ecrit des fichiers DAT au format Logiqx a partir de `mame -listxml`.
//
// Le catalogue en cache suffit a afficher la bibliotheque, mais pas au
// gestionnaire de ROMs : celui-ci compare fichier par fichier, et toute sa
// machinerie — DatParser, RomResolve, l'audit, la reparation, l'import —
// travaille sur des DAT Logiqx. Sans DAT, MAME resterait un catalogue qu'on
// regarde sans pouvoir rien reparer.
//
// MAME ne sort pas du Logiqx mais son propre format : on convertit donc, en
// flux, sans jamais charger les 320 Mo en memoire. Un fichier par famille de
// machines, sur le modele des DAT de FinalBurn Neo (un par systeme).
//
// Rend le nombre de fichiers ecrits, ou -1 en cas d'echec. `progress` recoit
// le nombre de machines traitees ; rendre false l'interrompt.
int generate_dats(const std::string& mame_exe,
                  const std::string& dat_dir,
                  const std::function<bool(int)>& progress = {});

// Le catalogue en memoire, sous la forme que l'interface manipule deja.
// Les machines purement internes (isdevice) sont ecartees : ce sont de vraies
// archives, necessaires a l'audit, mais personne ne les « joue ».
std::vector<Game> load(const std::shared_ptr<DatabaseManager>& db,
                       bool include_mechanical = false);

}  // namespace MameCatalog
