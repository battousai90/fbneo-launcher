// src/BootcadeAuth.h
//
// Connexion du joueur à son compte Bootcade, par le flux OAuth « code
// d'appareil » (RFC 8628).
//
// POURQUOI CE FLUX plutôt que le code d'autorisation classique : celui-ci
// exigerait que le launcher ouvre un serveur HTTP local le temps de recevoir
// la redirection, donc un port à choisir, un port qui peut être occupé, et un
// pare-feu local qui peut le bloquer. Le code d'appareil n'a besoin de rien de
// tout ça : le launcher affiche un code, le joueur le valide dans un
// navigateur, et le launcher interroge le serveur jusqu'à obtenir son jeton.
//
// Conséquence heureuse : ça fonctionne sur Steam Deck en mode gaming, où
// récupérer une redirection vers 127.0.0.1 est pénible, et le joueur peut
// même valider depuis son téléphone.
//
// Le launcher est un client PUBLIC : une application installée sur la machine
// du joueur ne peut rien garder de secret, donc il n'y a pas de secret client
// ici, et il ne faut pas en ajouter. PKCE remplace ce que le secret aurait
// apporté.
#pragma once

#include <string>

namespace BootcadeAuth {

// Ce que le serveur rend au début du parcours : le code à montrer au joueur,
// l'adresse où le valider, et le rythme auquel interroger.
struct DeviceCode {
    bool        ok = false;
    std::string error;              // rempli si ok == false

    std::string device_code;        // secret, ne s'affiche jamais
    std::string user_code;          // celui que le joueur lit, ex. XAWG-UAZI
    std::string verification_uri;   // adresse à saisir à la main
    std::string verification_uri_complete;  // la même, code déjà prérempli
    int         interval = 5;       // secondes entre deux interrogations
    int         expires_in = 600;   // durée de validité du code
};

// Une interrogation du serveur. Les trois premiers cas ne sont PAS des
// erreurs : ils font partie du déroulement normal.
enum class Poll {
    Pending,     // le joueur n'a pas encore validé : on continue
    SlowDown,    // on interroge trop vite : le serveur demande de ralentir
    Granted,     // c'est bon, les jetons sont enregistrés
    Denied,      // le joueur a refusé
    Expired,     // le code a expiré, il faut en redemander un
    Failed       // panne réseau ou réponse incompréhensible
};

// Démarre le parcours : demande un code au serveur.
DeviceCode begin();

// Une interrogation. À appeler toutes les `interval` secondes, en allongeant
// l'attente à chaque SlowDown.
Poll poll(const DeviceCode& dc);

// Restaure la session d'un lancement précédent à partir du jeton de
// rafraîchissement enregistré. Rend false s'il n'y en a pas ou s'il n'est plus
// valable : le joueur doit alors refaire le parcours.
//
// C'est ce qui évite de redemander une connexion à chaque démarrage.
bool restore();

// Un jeton d'accès valable, rafraîchi si nécessaire, ou une chaîne vide si
// personne n'est connecté. À appeler juste avant chaque requête plutôt que de
// garder le jeton : il expire en quelques minutes.
std::string access_token();

// Le nom du compte, lu dans le jeton. Vide si personne n'est connecté.
std::string username();

// L'avatar choisi par le joueur dans son compte, sous forme d'identifiant
// (« joystick », « cat »…) correspondant a assets/avatars/<id>.svg. Vide s'il
// n'en a pas choisi : l'appelant retombe alors sur une image par defaut.
//
// Les avatars sont EMBARQUES dans le launcher plutot que telecharges : ils
// pesent 176 Ko au total, ils doivent s'afficher hors ligne, et dependre du
// site pour dessiner sa propre interface serait fragile.
std::string avatar_id();

// Le pays declare dans le compte, en code ISO a deux lettres, ou vide.
std::string country();

bool signed_in();

// Oublie la session. N'invalide rien côté serveur : c'est une déconnexion
// locale, ce qui est ce qu'on attend d'un bouton dans une application.
void sign_out();

// Où la session est enregistrée. Exposé pour les tests et les diagnostics.
std::string session_path();

}  // namespace BootcadeAuth
