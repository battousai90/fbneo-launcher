# FBNeo-Launcher — écosystème web

Plan d'ensemble : ce qui reste sur Netlify, ce qui descend sur le homelab,
et ce qui est privé.

---

## Le principe de répartition

**Ne pas rapatrier ce qui marche déjà.** La landing page est sur Netlify :
CDN mondial, HTTPS gratuit, déploiement au push, disponible même quand le NUC
redémarre. Un serveur résidentiel derrière une IP dynamique ne fera jamais
mieux pour du HTML statique.

Le homelab prend ce que Netlify ne peut pas faire : le **volume**, l'**accès
privé**, et ce qui doit être **écrit** dynamiquement.

| Contenu | Où | Pourquoi |
|---|---|---|
| Landing page, doc, i18n | **Netlify** (déjà en place) | statique léger, CDN, rien à changer |
| Binaires du launcher | **GitHub Releases** (déjà en place) | versioning, checksums, bande passante gratuite |
| Catalogue de jeux (~25k) | **Netlify** | statique, voir §2 — pas besoin du homelab |
| Fichiers DAT | **Homelab, public** | volumineux, mis à jour souvent, consommés par le launcher |
| Miroir artwork | **Homelab, public** | plusieurs Go, ne rentre pas dans un free tier |
| ROMs homebrew / freeware | **Homelab, public** | distribuable, volumineux |
| Collection ROMs perso | **Homelab, privé** | authentifié, non indexé, accès nominatif |

---

## 1. Ce qui ne bouge pas

- `fbneo-launcher.netlify.app` — landing page, 7 langues, `build.js`
- GitHub Releases — `.AppImage`, `.deb`, `.flatpak`, `.tar.gz` + `SHA256SUMS`

Éventuellement : brancher `fbneo.gcourtot.duckdns.org` en CNAME vers Netlify
pour avoir un nom à toi sans héberger quoi que ce soit.

---

## 2. Le catalogue — l'interface « sexy »

C'est la pièce qui a le plus de valeur pour le projet, et elle peut être
**100 % statique**.

Tu as déjà `games.db` : 37 Mo de SQLite, ~25 000 jeux, avec système,
fabricant, année, type (Original / Clone / Hack / Homebrew / Bootleg /
Prototype), orientation, statut ROM. C'est exactement le modèle de données
d'un catalogue web.

**Approche recommandée — SQLite servi en statique.**
`sql.js-httpvfs` interroge un fichier `.db` posé sur un CDN via des requêtes
HTTP Range : le navigateur ne télécharge que les pages nécessaires, pas les
37 Mo. Tu réutilises ton schéma tel quel, aucune API, aucun backend.

Ce que ça donne côté visiteur : la même expérience que ton launcher — filtres
empilables, recherche instantanée, vue grille avec jaquettes. C'est la
meilleure démo possible du produit.

**Repli si trop complexe** : exporter un JSON compact (nom, année,
fabricant, système, type) et filtrer côté client avec un index de recherche.
Quelques Mo, plus simple, moins riche.

Les jaquettes du catalogue pointent vers le miroir artwork (§3).

---

## 3. Homelab public — `files.gcourtot.duckdns.org`

Un LXC nginx qui sert des fichiers statiques, alimenté depuis le NAS.

**Contenu :**

- `/dat/` — les DAT FBNeo, par version, plus un `manifest.json`
  (version, date, taille, SHA256) que le launcher peut interroger pour
  proposer une mise à jour. Ça remplace le téléchargement direct depuis
  GitHub par quelque chose que tu maîtrises.
- `/artwork/` — miroir preview/title, organisé par nom de set
- `/homebrew/` — ROMs homebrew et freeware, avec pour chaque entrée sa
  licence ou l'autorisation de l'auteur dans un fichier à côté

**Point d'intégration avec le launcher** : une fois `manifest.json` en place,
le launcher peut vérifier les mises à jour de DAT et d'artwork au démarrage.
C'est ce qui transforme un site vitrine en infrastructure du projet.

**Prudence sur le volume** : l'artwork complet FBNeo pèse plusieurs Go et
sera aspiré par des robots. Prévoir un `rate limit` nginx et surveiller la
bande passante montante.

---

## 4. Homelab privé — `roms.gcourtot.duckdns.org`

Filebrowser en Docker **sur le NAS** (pas sur le NUC : les 200 Go sont déjà
locaux là-bas, inutile de les faire transiter en CIFS).

- comptes nominatifs, lecture seule par défaut, un dossier racine par compte
- liens de partage avec mot de passe et expiration
- `robots.txt` bloquant, tout derrière login
- publication via NPM avec certificat Let's Encrypt

---

## 5. Vue d'ensemble

```
Netlify ──── landing page + catalogue (statique, CDN)
GitHub ───── binaires + releases

                    box (443)
                       │
                    NPM (CT 104)
                       ├── files.gcourtot.duckdns.org → CT 105 nginx
                       │      /dat  /artwork  /homebrew        (public)
                       └── roms.gcourtot.duckdns.org  → NAS:8080 Filebrowser
                              collection perso               (authentifié)
```

---

## 6. Ordre de réalisation

1. **CT 105 + nginx + `/dat/`** — le plus petit périmètre utile, et le plus
   directement utile au launcher
2. **Publication NPM + certificat** — valider la chaîne complète sur un
   contenu léger avant d'y mettre des Go
3. **`manifest.json` + support côté launcher** — l'intégration qui change tout
4. **Miroir artwork** — une fois la bande passante mesurée
5. **Filebrowser privé sur le NAS**
6. **Catalogue web** — le chantier le plus gros, indépendant des autres,
   à faire quand le reste tourne

Le point 1 est faisable dans la journée. Le point 6 est un projet en soi.

---

## 7. Décisions restées ouvertes

- **Débit montant réel** de la connexion — à mesurer avant de dimensionner
  quoi que ce soit de public
- **Le port 80** est-il ouvert par le FAI ? Sinon, certificats en DNS-01
- **Nom de domaine propre** ou rester sur DuckDNS
- **Catalogue** : `sql.js-httpvfs` ou export JSON
