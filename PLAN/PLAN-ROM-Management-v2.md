# ROM Management — plan de refonte v2

Consolidé le 2026-09-11 après audit du code et validation des 9 décisions.
Remplace `PLAN-ROM-Managment-redesign` (conservé comme brouillon d'origine).
Les maquettes `roms_managemen_redsign/*.png` restent la direction UX, pas un contrat.

Version : la refonte entre dans `1.1.0`, déjà déclarée dans CMakeLists et non
publiée. Pas de bump à faire. Tout se fait sur `Dev`, jamais sur `main`.

---

## 0. Décisions actées

| # | Sujet | Décision |
|---|---|---|
| 1 | Set strategy | **Split d'abord, Merged ensuite.** Corpus split de test + vérification que FBNeo le charge **avant** d'exposer l'option dans l'UI. |
| 2 | Tableaux | **Table plate + panneau bas listant les ROMs du set sélectionné** (nom attendu/trouvé, CRC attendu/trouvé, source de réparation). Plus d'arbre dépliable. |
| 3 | Ignored | **Vraie fonction persistante en DB** : « Ignore this set / do not report again ». |
| 4 | Chemins Outbox / Quarantine | **Settings**, carte « ROM Management ». Dans les tabs : seulement *Open folder*. |
| 5 | DAT dans Settings | **Retirés** (DAT Files + Generate DAT). Settings garde un lien *Manage DATs in ROM Management*. |
| 6 | Recherche locale de table | **Conservée** (Library, Import, Quarantine). La search bar globale du header reste supprimée. |
| 7 | Source HTTP DAT | **`manifest.json`** côté serveur : nom, taille, sha256, version, date. Check for updates = diff manifeste ↔ local. |
| 8 | Groupes DAT | **Modèle N groupes en DB/config, UI mono-groupe** (FinalBurn Neo) en v1. |
| 9 | Ordre | **Fondations (merge= + manifeste + composants UI) → Library → Import → Outbox → Quarantine → DAT.** |

Constats qui fondent ces choix :
- Le parser ignore `merge=` et `isbios` ; le scanner exige chaque ROM listée dans le
  zip du jeu → Bootcade ne comprend que le **non-merged**. 56 % des ROMs arcade
  (85 % Neo Geo) sont `merge=` : un set split est vu « missing ».
- Rien n'est persistant hors DB : ce qu'un Fix a fait, pourquoi un fichier est en
  quarantaine, d'où il vient — perdu au redémarrage. Les colonnes Reason / Original
  location / Fix performed exigent un **manifeste**.
- `apply()` n'écrit **jamais** un set incomplet dans l'outbox → pas de colonne Status
  dans Outbox, c'est cohérent.
- *Move to library* et la mise en quarantaine tournent **sur le thread GTK** → à
  passer en worker pendant la refonte.

---

## 1. Fondations (avant tout écran)

### 1a. `merge=` / `isbios` / style de set

**Modèle**
- `Rom::merge` (string, nom de l'entrée dans le set parent/BIOS) — `Game.h`, `DatParser.cpp`.
- `Game::is_bios` (bool) — attribut `isbios="yes"`.
- DB : `roms.merge TEXT`, `games.is_bios INTEGER` via le mécanisme `ALTER TABLE` existant.
  La migration pose un drapeau `scan_metadata.needs_dat_resync = 1` ; au lancement
  suivant, Bootcade propose la resync DAT (une fois) — sans elle `merge` reste vide et
  tout se comporte comme aujourd'hui (comportement non-merged).
- Réglage `set_style` **par groupe DAT** : `non-merged` (défaut = comportement actuel)
  | `split` | `merged` (phase ultérieure). Clé config `rom_manager.set_style`.

**Sémantique** (pour un clone `mslug2t` de `mslug2`, BIOS `neogeo`)

| style | `mslug2t.zip` contient | ROMs `merge=` cherchées dans |
|---|---|---|
| non-merged | tout | le zip du jeu (aujourd'hui) |
| split | ses ROMs propres seulement | `romof` → parent, puis BIOS (chaîne `romof`) |
| merged | *pas de zip* | le zip du parent contient parent + clones |

**Backend à toucher**
- `RomScanner` : la décision « available » d'un set split dépend d'*autres* zips
  (parent, BIOS). Le scan par zip ne suffit plus → évaluation en deux temps :
  collecte des contenus (déjà : `zip_contents`), puis résolution via
  `reevaluateGamesAvailability()` qui suit la chaîne `romof`. Une ROM `merge=`
  absente du zip du jeu **et** du parent/BIOS → Absent. Présente dans le parent →
  Present (source = parent).
- `RomAudit` : même règle ; nouvel état de détail « inherited from `<parent>.zip` ».
  Nouveau signal : **BIOS manquant** (`is_bios` absent → N sets dépendants injouables),
  affiché comme ligne dédiée dans Library.
- `RomInbox::apply` : style de sortie = « Set strategy » d'Import (défaut = style du
  groupe). En split : omettre les pièces `merge=` du zip du clone, et vérifier/produire
  le parent et le BIOS (sinon avertissement « parent absent : le set ne chargera pas »).
- `RomManagerWindow::verify_against_dat` (Move) : honore le style.

**État (2026-09-11)** — commits `bb42f2b` (modèle, parseur, DB, resync) et `9dddf4c`
(moteur `RomResolve`, scan, audit) :
- ✅ `merge=` / `isbios` lus et stockés ; relecture DAT proposée une fois.
- ✅ Règle unique `RomResolve::evaluate` ; NonMerged vérifié identique à l'ancien
  audit sur 29 527 sets ; Split validé sur corpus (parent → BIOS, `inherited_from` /
  `found_in`, ROM propre jamais satisfaite ailleurs, passe cache `touched` idempotente).
- ✅ Gate FBNeo passé : un set split (Neo Geo et GBA) se charge.
- ✅ **Limitation levée (2026-09-12, phase Import)** : `RomInbox` produit le style
  demandé (split : pièces `merge=` omises et non comptées manquantes, vérification
  post-reconstruction et « déjà en bibliothèque » sur les ROMs propres seulement) ;
  `verify_against_dat` de *Move to library* évalue avec `RomResolve` + `CacheIndex`
  de la bibliothèque (un zip split entre si son parent/BIOS est dans la bibliothèque).
  Note : la comparaison de **taille** de l'ancienne vérification est abandonnée au
  profit de la règle unique (nom + CRC), comme scan et audit.
- ⏳ Réglage `rom_manager.set_style` lu, mais aucune UI ne l'écrit encore
  (Library / DAT group, voir §2 et §6).

**Corpus de test (gate avant exposition UI)**
- Fabriquer dans un répertoire temporaire, à partir des sets non-merged de Gilbert :
  `neogeo.zip` (BIOS), `mslug2.zip` (parent, sans BIOS), `mslug2t.zip` (clone, ROMs
  propres seulement) + un cas console (`gba.zip` + un jeu GBA sans `gba_bios.bin`).
- Lancer FBNeo avec ce seul rom path sur le clone et sur le jeu GBA, vérifier le
  chargement (pas de « ROM missing » dans le log / la fenêtre).
- ✅ Passé le 2026-09-11 (témoins positif / négatif concluants). Attention : FBNeo
  écrit ses fichiers par jeu (`games/<rom>.ini`, autosave `.fs`) dans le vrai
  `~/.local/share/fbneo` même avec un `HOME` de test.

### 1b. Manifeste de mouvement

Fichier `.bootcade-manifest.json` à la racine de l'outbox et de la quarantaine.
Une entrée par fichier géré :

```json
{
  "file": "FinalBurn Neo - Neo Geo Games/mslug.zip",
  "game": "mslug", "system": "Neo Geo", "dat_header": "FinalBurn Neo - Neo Geo Games",
  "reason": "unknown | bad_crc | unsupported | duplicate | extra_files | replaced",
  "origin": "/mnt/1TB/ROMS/ToSort/Metal Slug (1996).zip",
  "action": "moved | rebuilt | renamed | extracted",
  "details": ["rebuilt from 3 files", "2 pieces from library: mslug2.zip"],
  "added_at": "2026-09-11T22:43:00Z"
}
```

- Écrit par : `RomInbox::apply` (outbox), les mises en quarantaine (Library, Import
  after-repair), *Move to library* (fichier remplacé → quarantaine `replaced`).
- Lu par : Outbox (colonne *Fix performed*), Quarantine (Reason, Original location,
  Date added, Details), *Restore to original location*.
- Fichier absent du manifeste (déposé à la main) → « manual », date = mtime.
- Historique : le manifeste garde les entrées supprimées dans `history[]` (borné,
  ex. 5 000 entrées) → base d'un « repair history » consultable plus tard.

**État (2026-09-11)** — module `RomManifest` (`schema_version: 1`, écriture atomique
tmp+rename, `add/find/remove/reconcile/retire_all`, issues `moved_to_library /
restored / deleted / purged / vanished`). Écrit par `RomInbox::apply` (outbox : `moved`
/ `rebuilt` avec le détail des pièces), la quarantaine de Library (`bad_crc`,
`extra_files` via `RomCleanup`), *Move to library* (retire → `moved_to_library`) et
*Empty quarantine* (contenu supprimé, manifeste réécrit avec tout en `purged`).
Lecture par les écrans : à faire avec Outbox / Quarantine. `replaced` : avec l'option
Outbox. `unknown` / `unsupported` / `duplicate` : avec le panneau *After repair* d'Import.

**Bug préexistant relevé — corrigé en phase Import (2026-09-12)** : un set présent dans deux DAT
(tout Neo Geo est aussi dans Arcade) est *Moved* une première fois puis le second
*Move* échoue (« No such file ») : `Relocations` ne couvre que les *Rebuild*. Le second
DAT devrait copier depuis la destination du premier.

### 1c. Sets ignorés

- Table `ignored_sets(name, system, note, added_at)`, `UNIQUE(name, system)`.
- L'audit les exclut des problèmes et les compte dans *Ignored*. Menu contextuel
  Library : *Ignore this set* / *Stop ignoring*. Filtre *Ignored* pour les revoir.
- **État (2026-09-12, passe « groupes réels »)** — tout ce qui précède est codé
(`DatSource`, `RomDatTab`, `RomLibraryTab`, `RomAudit` filtré, `RomResolve`,
`DATUpdateDialog` sur liste explicite, `MainWindow` sérialisé). Cosmétique commune :
`check`/`radio` (boutons **et** cellules de tableau), barres de progression, anneau de
focus, bouton ⋯ de ligne → règles `.set-window` dans `style-common.css` /
`style-dark.css` ; plus aucune couleur du thème du bureau dans les 5 onglets.
**Vérifié runtime** (app complète, HOME scratch, dossier DAT scratch, serveur réel) :
migration `inactive_files`, trois groupes sur la même source HTTP (FinalBurn Neo 18,
FBNeo - GBA 1, Special Arcade 2) avec sélections différentes, changement de groupe →
table, Library : combo = groupes actifs, audit « Compared with 2 DAT files (Special
Arcade) » = 9 062 sets, GBA = 3 242 ; persistance après redémarrage ; Rename / Disable
(le groupe disparaît du combo Library, repli persisté) / Enable / Delete avec
confirmation ; rechargement différé et conditionnel (union) ; Check + Download limités
au groupe (fichier GBA supprimé → « not downloaded yet » → retéléchargé seul, les
autres intacts, aucun temporaire) ; harnais `h8` (sélection, ids, union/conflits,
migrations, aller-retour config). **À VÉRIFIER** : *Generate from FBNeo* depuis
l'onglet (écrit dans le vrai `~/.local/share/fbneo`), *Add DAT files…* (copie + prise
dans le groupe), Browse (changement de dossier), deux groupes sur deux dossiers
différents avec un même nom de fichier (conflit), rendu du dialogue `DATUpdateDialog`
(hérité, hors charte : barre de progression du thème).

---

## 2. Library

**Rôle** : scanner, inventorier, comparer au DAT, montrer les écarts. Ne lance rien.

**Header de carte** : DAT group (combo, mono-groupe en v1 : « FinalBurn Neo (17 DAT
files) ») · *Scan ROMs* (bouton principal, = ROMScanDialog existant) · *Audit library*.
Pas de badge « Active » (doublon du tab DAT).

**Résumé** : pills cliquables (= filtre) *Total · Correct · Missing · Incorrect ·
Fixable · Orphan · Ignored* + « Last audit: date — compared with N DAT files ».
Ligne BIOS si un BIOS manque : « neogeo BIOS missing — 312 sets cannot run ».

**Filtres** : recherche locale (game, rom, filename, parent, crc) · System. Pas de
combo Status ni Show : les pills font les deux (Correct désactivée par défaut =
« problems only » ; l'activer = « all sets »).

**Table (plate, une ligne par set)** : ☐ · Status · Game / ROM (nom + description) ·
System · Parent · Expected file · Your file · Details. Scroll, pas de pagination.
Pas de CRC/Size au niveau set.

**Panneau bas « Selected set »** : liste des ROMs du set : State · Expected name ·
Found as · CRC expected · CRC found · Size · Found in (autre archive de la lib, ou
parent/BIOS en split). C'est là que vivent les CRC.

**Menu contextuel** : Copy game name · Copy expected filename · Copy your filename ·
Copy parent name · Copy expected CRC / found CRC (sur une ROM du panneau) ·
Copy all details · Copy missing filenames · Search on web (`xdg-open` d'une
recherche DuckDuckGo « <expected filename> <system> », non configurable en v1) · Ignore this set / Stop ignoring ·
Send to Import (sets Fixable) · Quarantine (sets irréparables / orphelins / extras).

**Boutons bas** : *Send fixable to Import* (rend visible le pull que fait déjà
Analyse) · *Quarantine selected* (l'actuel « Fix », renommé : irréparable → quarantaine
`bad_crc` ; orphelin → inbox pour ré-identification ; extras → extraits `extra_files`)
· *Export…* (dropdown : Text / CSV / Missing sets as DAT).

Supprimé du mockup : colonne *Clone of*, lien *How to fix common issues*, combo *Show*.

**État (2026-09-12)** — `RomLibraryTab` (classe autonome, worker + dispatcher propres)
branchée dans `RomManagerWindow`, dont le chrome est passé à la charte Settings
(barre de titre à tuile accent, onglets `set-tab`, sous-titre par onglet). Les quatre
autres onglets gardent leur contenu d'avant jusqu'à leur tour. `--open=roms` ouvre
la fenêtre pour l'automatisation. Vérifié sur la bibliothèque réelle (copie) :
audit 29 527 sets, pills-filtres, table plate 29k lignes, panneau ROMs, exports
(texte / CSV / DAT des manquants), menu contextuel, ignore, quarantaine en worker.
**À VÉRIFIER (runtime)** : *Send fixable to Import* et *Quarantine selected* de bout
en bout (la bibliothèque de test n'a ni set réparable ni set irréparable), *Scan ROMs*
réel depuis l'onglet. La logique reprend l'existant.

---

## 3. Import

**Rôle** : analyser et réparer des fichiers apportés. N'écrit jamais dans la Library ;
tout sort vers Outbox.

**Carte Import source** : chemin + Browse · Scan subdirectories · Include archives ·
Include loose files.

**Carte Repair options** :
- ☑ Use existing Library ROMs (pool CRC ; décoché = inbox seule)
- Set strategy : *Same as library* | Non-merged | Split *(caché tant que 1a non validé)*
- *Output: ZIP* (info fixe, pas un choix — le scanner de bibliothèque ne lit que du ZIP)
- ☐ Rebuild already-correct sets (normalisation ZIP) — option secondaire
- Compression level (Store / Normal / Max) — option secondaire

Supprimés : Use files from Import, Use parent/clone/BIOS ROMs, Allow cross-system,
Extract archives if needed, Process loose ROM files, Preferred archive format.
La provenance des pièces se **montre** (colonne Repair source) au lieu de se cocher.

**Carte After repair** : ○ Move processed source files to a subfolder (`_processed/`,
défaut) · ○ Delete processed source files (comportement actuel, devient un choix) ·
☑ Move unknown / unsupported files to Quarantine.

**Résumé** : pills cliquables *Valid · Fixable · Missing · Unknown · Already in
library · Ignored (non-ROM) · Total*.

**Table plate** : ☐ · Status · Game / ROM · System · File name · Size · Repair source
(Import / Library / mixed) · Details.
**Panneau bas** : ROMs du set : Expected name · Found as · CRC expected · CRC found ·
Source (fichier + entrée) · Action (move / rename / from library / missing).

**Log** : `ui::log_panel`.
**Bas** : *Analyse* · *Fix selected (N)* ▾ (*Fix all fixable*) · *Cancel* · *Export missing list* ▾ (Text / CSV / DAT).

**État (2026-09-12)** — `RomImportTab` branchée ; ancien Import retiré de la fenêtre
(~700 lignes). `RomInbox::Options` : récursif / archives / loose / **use_library** /
**rebuild_correct** / **style** / **processed** (`_processed/` défaut, delete, keep) /
**quarantine_rejects** (unknown, unsupported, duplicate → `<quarantaine>/_rejected/` +
manifeste). Fichiers non-ROM comptés (*Ignored*). Bug du double *Move* corrigé (copie
depuis la destination du premier). Détail et journal côte à côte. Compression level :
non retenu (valeur faible). **Vérifié runtime** (harnais corpus A/B/C + app complète
sur base copie) : Analyse, pastilles, panneau des pièces, Fix + confirmation, rejets
en quarantaine avec raison, sources parquées, sortie split (1 ROM propre), double DAT.
**À VÉRIFIER** : *Fix all fixable*, *Export missing list*, *receive()* depuis Library.

---

## 4. Outbox

**Rôle** : sas de validation. Contenu déjà prêt (garanti par `apply()`).

**Carte Move options** :
- ☑ Keep replaced files in Quarantine (défaut ON)
- If destination exists : Replace | Skip if identical | Skip
- Info : « Every set is verified against the DAT before moving. » (toujours, non désactivable)
- **Destination mapping** : bouton *Edit…* → table System → dossier de destination,
  pré-remplie par correspondance de nom avec `roms_paths`, éditable, persistée
  (`rom_manager.destinations`). Règle le cas des sets « unmapped ».

Supprimés : After repair, Compress loose, Recompress, Archive format, Compression
level, Organize in DAT folders, Folder pattern, Verify (comme case), Analyse.

**Table plate** : ☐ · Game / ROM · System · Archive · Parent · Files (n/n) ·
Size · Destination (= dry-run : `→ …/Arcade Games/mslug.zip (will replace)` /
`no destination mapped`) · Fix performed (manifeste).
**Panneau bas** : ROMs du zip : Name · CRC · Size · Source.

**Log** : `ui::log_panel`.
**Bas** : *Open folder* · *Refresh* (+ auto-refresh à l'affichage du tab) ·
*Move selected (N)* ▾ (*Move all*) · *Cancel* (worker).

**État (2026-09-12)** — `RomOutboxTab` branchée, ancien Outbox retiré (~310 lignes).
Options réelles : *Keep replaced in Quarantine* (→ `<quarantaine>/_replaced/<système>/`,
manifeste `replaced`, origine = chemin bibliothèque), politique de collision
(*Replace* / *Skip when identical, replace otherwise* / *Skip*), **Destination mapping**
(dialogue système → dossier, défaut = nom identique, clé `rom_manager.destinations`).
Colonnes : Game · System · Archive · Parent · Files n/n · Size · **Destination** (dry-run,
« (exists) ») · **Fix performed** (manifeste). Pills Total / Ready / No destination. Détail :
enregistrement du manifeste + entrées du zip. Move en **worker** (Cancel), vérification
`RomResolve` + index bibliothèque, dossiers vides supprimés, rescan demandé.
**§1e fait en même temps** : carte *ROM Management* dans Settings › Library (Outbox /
Quarantine + lien *Manage DATs in ROM Management* qui ouvre l'onglet DAT). Le retrait de
*DAT Files* / *Generate DAT* de Settings attend l'écran DAT.
**Vérifié runtime** (app complète, bibliothèque scratch) : remplacement + conservation,
identique écarté, manifestes, journal. **À VÉRIFIER** : *Move all*, dialogue de mapping,
politique *Skip*, un set refusé par la vérification DAT.

---

## 5. Quarantine

**Rôle** : rejets. Simple.

**Header** : *Restore selected to Import* ▾ (*Restore to original location* si le
manifeste le sait) · *Delete selected* · *Empty quarantine* (danger).
**Pills cliquables** : All · Unknown · Bad CRC · Unsupported · Duplicate · Extra files · Replaced.
**Filtres** : recherche locale · System.
**Table** : ☐ · Reason · Game / File · System · Original location · File name ·
Size · Date added · Details.
**Bas** : *Open folder*. Pas de log.

**État (2026-09-12)** — `RomQuarantineTab` branchée, ancien Quarantine retiré (~170
lignes), chemin lu dans Settings. Pills-filtres par raison (Unknown / Bad CRC /
Unsupported / Duplicate / Extra files / Replaced / **No record**), table Reason · Game /
File · System · Original location · File name · Size · Date added · Details (manifeste ;
« no record : not put here by Bootcade » sinon, date = mtime). *Restore selected to
Import* ▾ (*to original location* : seulement pour un fichier déplacé entier, jamais
une entrée extraite ; refusé si la place est reprise), *Delete selected* et *Empty
quarantine* avec confirmation, *Open folder* / *Refresh*, rafraîchissement à
l'affichage de l'onglet (Outbox aussi). Opérations synchrones (pas de worker : petits
déplacements), pas de journal propre (lignes envoyées au journal Import).
**Vérifié runtime** : restauration vers Import (4 fichiers, historique `restored`),
suppression avec confirmation, vidage conservant le manifeste, lignes « No record ».
**À VÉRIFIER** : *Restore to original location*, cocher une ligne à la souris (l'outil
d'automatisation ne sait pas actionner une case de TreeView — même chemin que
Library/Import).

---

## 6. DAT

**Rôle** : référentiels uniquement.

**Modèle (N groupes, réel — révisé 2026-09-12)**
- Un **DAT group** est une **sélection nommée de fichiers DAT**, organisée par
  l'utilisateur : « FinalBurn Neo » (les 19), « FBNeo - GBA » (un seul), « Special
  Arcade » (choisis à la main). Ce n'est **ni un émulateur ni une source**.
- Une **source** (executable / HTTP / dossier) alimente un dossier ; **plusieurs groupes
  peuvent partager la même source** et le même dossier, chacun avec sa sélection. Les
  fichiers sont stockés une fois.
- `rom_manager.dat_groups[]` dans config.json : `id` (stable, jamais affiché), `name`,
  `folder`, `source` (`emulator`|`http`|`folder`), `url`, `set_style`, `active`,
  `all_files` + `files[]` (la sélection), `last_check`, `last_update`.
- **La base = union des fichiers sélectionnés par les groupes actifs**, chaque fichier
  chargé une fois (`DatSource::files_to_load`) ; deux dossiers différents pour un même
  nom de fichier → le premier groupe gagne, conflit journalisé.
- **Library** audite contre **un groupe** (combo = groupes actifs, choix persisté dans
  `rom_manager.library_group`) : total, verdicts et BIOS ne portent que sur les jeux
  des DAT de ce groupe. `RomResolve::load_style()` lit le `set_style` de ce groupe.
- Migration : pas de `dat_groups` → groupe « FinalBurn Neo » depuis `dat_path`
  (source http par défaut, style = ancien `rom_manager.set_style`) ; ancienne forme
  `inactive_files` → `all_files:false` + liste explicite.

**Source HTTP**
- **`dat-manifest.json`** sur `files.gcourtot.duckdns.org/dat/` — contrat v1 **figé
  le 2026-09-12** (homelab `f7b9499`, `scripts/generate-dat-manifest.sh`) :
  ```json
  { "schema_version": 1, "generated": "2026-09-12T06:15:00Z", "group": "FinalBurn Neo",
    "files": [ { "name": "FBNeo_-_Arcade.dat", "size": 14717725, "sha256": "…",
                 "version": "1.0.0.03", "date": "2026-09-08T15:30:05Z" } ] }
  ```
  - Produit sur **CT105** (source de vérité de ce qui est servi), par
    `fetch-fbneo-dats.sh` juste après chaque dépôt + cron */15 en filet. Le fork
    FBNeo reste producteur de DAT seulement.
  - **sha256 = seul critère machine de changement** ; `version` = version FBNeo lue
    dans l'en-tête (identique pour tous les fichiers d'une build, inchangée quand le
    contenu change → informative) ; `date` = mtime = dernière révision servie.
  - Les fichiers sont à côté du manifeste (pas de `base_url`).
  - Réservé à Bootcade : le site lit ses propres métadonnées dans
    `catalog-data.json` (`dats[]`, homelab `98a3905`, bootcade `46a43c7`).
    L'ancien `manifest.json` n'est plus produit.
  - **Client** : refuse un `schema_version` inconnu (message clair, rien téléchargé) ;
    après téléchargement, **vérifie le sha256 avant d'accepter le fichier** (écart =
    « a changé sous moi, réessayer », jamais un fichier douteux dans le dossier DAT).
- *Check for updates* : télécharge le manifeste, compare sha256/version au local,
  affiche « local 1.0.0.03 → remote 1.0.0.04 ». *Download* ne prend que ce qui diffère,
  vérifie le sha256, puis resync DB automatique.
- Option *Check for DAT updates at startup* (notification, jamais d'application
  automatique).

**UI (multi-groupes)**
- Colonne gauche **DAT groups** : une ligne par groupe (nom, « N files · N sets »,
  pastille active/inactive, menu ⋯ *Rename… / Enable-Disable group / Delete group…*
  avec confirmation, refusé pour le dernier groupe), *+ Add group* (dialogue Bootcade ;
  le nouveau groupe copie source/dossier/style du groupe courant et prend tous les
  fichiers).
- Tout le reste de l'écran = le groupe sélectionné : carte du groupe (dossier, **set
  style**, bouton principal selon la source, *Check for updates*, *Add DAT files…*, ⋯),
  carte Source (radio Emulator / HTTP / Folder, URL, indication), table avec colonne
  **In group** (☑ = fichier dans la sélection ; les fichiers du serveur pas encore
  téléchargés apparaissent aussi, « not downloaded yet »), panneaux DAT information /
  Source information (**Shared with** = autres groupes sur la même source) / preview.
- *Download DATs* ne prend que les fichiers **sélectionnés par le groupe** qui manquent
  ou diffèrent (sha256). Les autres groupes ne sont pas touchés.
- Rechargement de la base **différé (2,5 s après le dernier clic)** et **seulement si
  l'union chargée change** (un fichier qu'un autre groupe charge déjà ne déclenche
  rien) ; `MainWindow::do_update_dat` sérialise les demandes (jamais deux dialogues).
- Footer : « N of M DAT files in this group · sets · ROM entries ».

**État (2026-09-12)** : table `ignored_sets` (propre, sans clé étrangère : survit au
  rechargement DAT, qui ne vide que `games`/`roms`), API `ignoreSet / unignoreSet /
  isIgnored / getIgnoredSets`, audit : `GameEntry.ignored`, compteur `Report.ignored`,
  hors Missing/Incorrect/Fixable, jamais réparable, toujours listé. Testé sur harnais.
  UI (menu contextuel, badge, filtre) : avec l'écran Library.
- **Tranché** : les sets ignorés **restent visibles** dans la bibliothèque principale
  (ignorer = « ne plus me le signaler comme problème », pas « cacher »), avec un badge
  *Ignored* et un filtre pour les retrouver.

### 1d. Composants UI communs

Réutiliser `SettingsUi` (`ui::card`, `ui::rows/row`, `ui::button`, `ui::status_*`,
`ui::hairline`, classes `.set-*`) et le chrome de fenêtre de Settings/Controller
(`set-window`, tuile accent dans l'en-tête, `set-tabbar`/`set-tab`, `cc-footer`).
À ajouter dans `SettingsUi` (pas de CSS propre à ROM Management) :

- `ui::pill(label, count, tone)` : badge de compteur fond faiblement teinté +
  bordure + texte colorés, **cliquable = filtre** (état `on`).
- `ui::log_panel()` : TextView monospace + *Export log* / *Clear log* / *Auto-scroll*.
  Un seul composant pour Import et Outbox.
- `ui::table()` : TreeView + ScrolledWindow avec en-tête, lignes zébrées, colonne
  case-à-cocher, tri par colonne, classe commune.
- `ui::filter_bar()` : recherche locale + combos System/Show + compteur « N results ».
- `ui::detail_panel()` : carte basse à hauteur fixe, liste de ROMs.
- Icônes : une seule famille (assets/icons), les mêmes que Settings. Une icône par
  tab, identique partout.
- Chaînes via `_()` et le système i18n JSON existant.

### 1e. Settings

- Nouvelle carte **ROM Management** : *Outbox directory*, *Quarantine directory*
  (clés existantes `rom_manager.outbox_path` / `quarantine_path`), et une ligne
  *DAT files — Manage in ROM Management* (bouton qui ouvre le tab DAT).
- Retirer *DAT Files* (chemin) et *Generate DAT*. Le signal `signal_dat_path_changed`
  et la synchro Settings↔RomManager deviennent inutiles.
- Conserver *ROM Directories* et *Scan Options* (récursif / loose) : Import et
  Library les lisent.

### 1f. Dette technique à régler en passant

- *Move to library* et la mise en quarantaine → worker + progression + Cancel
  (même plumbing que Analyse/Fix : `std::thread` + `Glib::Dispatcher`).
- *Update database from DAT files* → automatique après Generate / Download / ajout
  de fichiers ; manuel dans le menu ⋮ du tab DAT.

---

## 2. Library

**Rôle** : scanner, inventorier, comparer au DAT, montrer les écarts. Ne lance rien.

**Header de carte** : DAT group (combo, mono-groupe en v1 : « FinalBurn Neo (17 DAT
files) ») · *Scan ROMs* (bouton principal, = ROMScanDialog existant) · *Audit library*.
Pas de badge « Active » (doublon du tab DAT).

**Résumé** : pills cliquables (= filtre) *Total · Correct · Missing · Incorrect ·
Fixable · Orphan · Ignored* + « Last audit: date — compared with N DAT files ».
Ligne BIOS si un BIOS manque : « neogeo BIOS missing — 312 sets cannot run ».

**Filtres** : recherche locale (game, rom, filename, parent, crc) · System. Pas de
combo Status ni Show : les pills font les deux (Correct désactivée par défaut =
« problems only » ; l'activer = « all sets »).

**Table (plate, une ligne par set)** : ☐ · Status · Game / ROM (nom + description) ·
System · Parent · Expected file · Your file · Details. Scroll, pas de pagination.
Pas de CRC/Size au niveau set.

**Panneau bas « Selected set »** : liste des ROMs du set : State · Expected name ·
Found as · CRC expected · CRC found · Size · Found in (autre archive de la lib, ou
parent/BIOS en split). C'est là que vivent les CRC.

**Menu contextuel** : Copy game name · Copy expected filename · Copy your filename ·
Copy parent name · Copy expected CRC / found CRC (sur une ROM du panneau) ·
Copy all details · Copy missing filenames · Search on web (`xdg-open` d'une
recherche DuckDuckGo « <expected filename> <system> », non configurable en v1) · Ignore this set / Stop ignoring ·
Send to Import (sets Fixable) · Quarantine (sets irréparables / orphelins / extras).

**Boutons bas** : *Send fixable to Import* (rend visible le pull que fait déjà
Analyse) · *Quarantine selected* (l'actuel « Fix », renommé : irréparable → quarantaine
`bad_crc` ; orphelin → inbox pour ré-identification ; extras → extraits `extra_files`)
· *Export…* (dropdown : Text / CSV / Missing sets as DAT).

Supprimé du mockup : colonne *Clone of*, lien *How to fix common issues*, combo *Show*.

**État (2026-09-12)** — `RomLibraryTab` (classe autonome, worker + dispatcher propres)
branchée dans `RomManagerWindow`, dont le chrome est passé à la charte Settings
(barre de titre à tuile accent, onglets `set-tab`, sous-titre par onglet). Les quatre
autres onglets gardent leur contenu d'avant jusqu'à leur tour. `--open=roms` ouvre
la fenêtre pour l'automatisation. Vérifié sur la bibliothèque réelle (copie) :
audit 29 527 sets, pills-filtres, table plate 29k lignes, panneau ROMs, exports
(texte / CSV / DAT des manquants), menu contextuel, ignore, quarantaine en worker.
**À VÉRIFIER (runtime)** : *Send fixable to Import* et *Quarantine selected* de bout
en bout (la bibliothèque de test n'a ni set réparable ni set irréparable), *Scan ROMs*
réel depuis l'onglet. La logique reprend l'existant.

---

## 3. Import

**Rôle** : analyser et réparer des fichiers apportés. N'écrit jamais dans la Library ;
tout sort vers Outbox.

**Carte Import source** : chemin + Browse · Scan subdirectories · Include archives ·
Include loose files.

**Carte Repair options** :
- ☑ Use existing Library ROMs (pool CRC ; décoché = inbox seule)
- Set strategy : *Same as library* | Non-merged | Split *(caché tant que 1a non validé)*
- *Output: ZIP* (info fixe, pas un choix — le scanner de bibliothèque ne lit que du ZIP)
- ☐ Rebuild already-correct sets (normalisation ZIP) — option secondaire
- Compression level (Store / Normal / Max) — option secondaire

Supprimés : Use files from Import, Use parent/clone/BIOS ROMs, Allow cross-system,
Extract archives if needed, Process loose ROM files, Preferred archive format.
La provenance des pièces se **montre** (colonne Repair source) au lieu de se cocher.

**Carte After repair** : ○ Move processed source files to a subfolder (`_processed/`,
défaut) · ○ Delete processed source files (comportement actuel, devient un choix) ·
☑ Move unknown / unsupported files to Quarantine.

**Résumé** : pills cliquables *Valid · Fixable · Missing · Unknown · Already in
library · Ignored (non-ROM) · Total*.

**Table plate** : ☐ · Status · Game / ROM · System · File name · Size · Repair source
(Import / Library / mixed) · Details.
**Panneau bas** : ROMs du set : Expected name · Found as · CRC expected · CRC found ·
Source (fichier + entrée) · Action (move / rename / from library / missing).

**Log** : `ui::log_panel`.
**Bas** : *Analyse* · *Fix selected (N)* ▾ (*Fix all fixable*) · *Cancel* · *Export missing list* ▾ (Text / CSV / DAT).

**État (2026-09-12)** — `RomImportTab` branchée ; ancien Import retiré de la fenêtre
(~700 lignes). `RomInbox::Options` : récursif / archives / loose / **use_library** /
**rebuild_correct** / **style** / **processed** (`_processed/` défaut, delete, keep) /
**quarantine_rejects** (unknown, unsupported, duplicate → `<quarantaine>/_rejected/` +
manifeste). Fichiers non-ROM comptés (*Ignored*). Bug du double *Move* corrigé (copie
depuis la destination du premier). Détail et journal côte à côte. Compression level :
non retenu (valeur faible). **Vérifié runtime** (harnais corpus A/B/C + app complète
sur base copie) : Analyse, pastilles, panneau des pièces, Fix + confirmation, rejets
en quarantaine avec raison, sources parquées, sortie split (1 ROM propre), double DAT.
**À VÉRIFIER** : *Fix all fixable*, *Export missing list*, *receive()* depuis Library.

---

## 4. Outbox

**Rôle** : sas de validation. Contenu déjà prêt (garanti par `apply()`).

**Carte Move options** :
- ☑ Keep replaced files in Quarantine (défaut ON)
- If destination exists : Replace | Skip if identical | Skip
- Info : « Every set is verified against the DAT before moving. » (toujours, non désactivable)
- **Destination mapping** : bouton *Edit…* → table System → dossier de destination,
  pré-remplie par correspondance de nom avec `roms_paths`, éditable, persistée
  (`rom_manager.destinations`). Règle le cas des sets « unmapped ».

Supprimés : After repair, Compress loose, Recompress, Archive format, Compression
level, Organize in DAT folders, Folder pattern, Verify (comme case), Analyse.

**Table plate** : ☐ · Game / ROM · System · Archive · Parent · Files (n/n) ·
Size · Destination (= dry-run : `→ …/Arcade Games/mslug.zip (will replace)` /
`no destination mapped`) · Fix performed (manifeste).
**Panneau bas** : ROMs du zip : Name · CRC · Size · Source.

**Log** : `ui::log_panel`.
**Bas** : *Open folder* · *Refresh* (+ auto-refresh à l'affichage du tab) ·
*Move selected (N)* ▾ (*Move all*) · *Cancel* (worker).

**État (2026-09-12)** — `RomOutboxTab` branchée, ancien Outbox retiré (~310 lignes).
Options réelles : *Keep replaced in Quarantine* (→ `<quarantaine>/_replaced/<système>/`,
manifeste `replaced`, origine = chemin bibliothèque), politique de collision
(*Replace* / *Skip when identical, replace otherwise* / *Skip*), **Destination mapping**
(dialogue système → dossier, défaut = nom identique, clé `rom_manager.destinations`).
Colonnes : Game · System · Archive · Parent · Files n/n · Size · **Destination** (dry-run,
« (exists) ») · **Fix performed** (manifeste). Pills Total / Ready / No destination. Détail :
enregistrement du manifeste + entrées du zip. Move en **worker** (Cancel), vérification
`RomResolve` + index bibliothèque, dossiers vides supprimés, rescan demandé.
**§1e fait en même temps** : carte *ROM Management* dans Settings › Library (Outbox /
Quarantine + lien *Manage DATs in ROM Management* qui ouvre l'onglet DAT). Le retrait de
*DAT Files* / *Generate DAT* de Settings attend l'écran DAT.
**Vérifié runtime** (app complète, bibliothèque scratch) : remplacement + conservation,
identique écarté, manifestes, journal. **À VÉRIFIER** : *Move all*, dialogue de mapping,
politique *Skip*, un set refusé par la vérification DAT.

---

## 5. Quarantine

**Rôle** : rejets. Simple.

**Header** : *Restore selected to Import* ▾ (*Restore to original location* si le
manifeste le sait) · *Delete selected* · *Empty quarantine* (danger).
**Pills cliquables** : All · Unknown · Bad CRC · Unsupported · Duplicate · Extra files · Replaced.
**Filtres** : recherche locale · System.
**Table** : ☐ · Reason · Game / File · System · Original location · File name ·
Size · Date added · Details.
**Bas** : *Open folder*. Pas de log.

**État (2026-09-12)** — `RomQuarantineTab` branchée, ancien Quarantine retiré (~170
lignes), chemin lu dans Settings. Pills-filtres par raison (Unknown / Bad CRC /
Unsupported / Duplicate / Extra files / Replaced / **No record**), table Reason · Game /
File · System · Original location · File name · Size · Date added · Details (manifeste ;
« no record : not put here by Bootcade » sinon, date = mtime). *Restore selected to
Import* ▾ (*to original location* : seulement pour un fichier déplacé entier, jamais
une entrée extraite ; refusé si la place est reprise), *Delete selected* et *Empty
quarantine* avec confirmation, *Open folder* / *Refresh*, rafraîchissement à
l'affichage de l'onglet (Outbox aussi). Opérations synchrones (pas de worker : petits
déplacements), pas de journal propre (lignes envoyées au journal Import).
**Vérifié runtime** : restauration vers Import (4 fichiers, historique `restored`),
suppression avec confirmation, vidage conservant le manifeste, lignes « No record ».
**À VÉRIFIER** : *Restore to original location*, cocher une ligne à la souris (l'outil
d'automatisation ne sait pas actionner une case de TreeView — même chemin que
Library/Import).

---

## 6. DAT

**Rôle** : référentiels uniquement.

**Modèle (N groupes, réel — révisé 2026-09-12)**
- Un **DAT group** est une **sélection nommée de fichiers DAT**, organisée par
  l'utilisateur : « FinalBurn Neo » (les 19), « FBNeo - GBA » (un seul), « Special
  Arcade » (choisis à la main). Ce n'est **ni un émulateur ni une source**.
- Une **source** (executable / HTTP / dossier) alimente un dossier ; **plusieurs groupes
  peuvent partager la même source** et le même dossier, chacun avec sa sélection. Les
  fichiers sont stockés une fois.
- `rom_manager.dat_groups[]` dans config.json : `id` (stable, jamais affiché), `name`,
  `folder`, `source` (`emulator`|`http`|`folder`), `url`, `set_style`, `active`,
  `all_files` + `files[]` (la sélection), `last_check`, `last_update`.
- **La base = union des fichiers sélectionnés par les groupes actifs**, chaque fichier
  chargé une fois (`DatSource::files_to_load`) ; deux dossiers différents pour un même
  nom de fichier → le premier groupe gagne, conflit journalisé.
- **Library** audite contre **un groupe** (combo = groupes actifs, choix persisté dans
  `rom_manager.library_group`) : total, verdicts et BIOS ne portent que sur les jeux
  des DAT de ce groupe. `RomResolve::load_style()` lit le `set_style` de ce groupe.
- Migration : pas de `dat_groups` → groupe « FinalBurn Neo » depuis `dat_path`
  (source http par défaut, style = ancien `rom_manager.set_style`) ; ancienne forme
  `inactive_files` → `all_files:false` + liste explicite.

**Source HTTP**
- **`dat-manifest.json`** sur `files.gcourtot.duckdns.org/dat/` — contrat v1 **figé
  le 2026-09-12** (homelab `f7b9499`, `scripts/generate-dat-manifest.sh`) :
  ```json
  { "schema_version": 1, "generated": "2026-09-12T06:15:00Z", "group": "FinalBurn Neo",
    "files": [ { "name": "FBNeo_-_Arcade.dat", "size": 14717725, "sha256": "…",
                 "version": "1.0.0.03", "date": "2026-09-08T15:30:05Z" } ] }
  ```
  - Produit sur **CT105** (source de vérité de ce qui est servi), par
    `fetch-fbneo-dats.sh` juste après chaque dépôt + cron */15 en filet. Le fork
    FBNeo reste producteur de DAT seulement.
  - **sha256 = seul critère machine de changement** ; `version` = version FBNeo lue
    dans l'en-tête (identique pour tous les fichiers d'une build, inchangée quand le
    contenu change → informative) ; `date` = mtime = dernière révision servie.
  - Les fichiers sont à côté du manifeste (pas de `base_url`).
  - Réservé à Bootcade : le site lit ses propres métadonnées dans
    `catalog-data.json` (`dats[]`, homelab `98a3905`, bootcade `46a43c7`).
    L'ancien `manifest.json` n'est plus produit.
  - **Client** : refuse un `schema_version` inconnu (message clair, rien téléchargé) ;
    après téléchargement, **vérifie le sha256 avant d'accepter le fichier** (écart =
    « a changé sous moi, réessayer », jamais un fichier douteux dans le dossier DAT).
- *Check for updates* : télécharge le manifeste, compare sha256/version au local,
  affiche « local 1.0.0.03 → remote 1.0.0.04 ». *Download* ne prend que ce qui diffère,
  vérifie le sha256, puis resync DB automatique.
- Option *Check for DAT updates at startup* (notification, jamais d'application
  automatique).

**UI v1 (mono-groupe, la colonne « DAT groups » viendra avec un 2ᵉ émulateur)**
- Header : nom du groupe · dossier · set style · **un** bouton principal dépendant
  de la source (*Generate from FBNeo* / *Download DATs* / *Rescan folder*) ·
  *Check for updates* (HTTP seulement) · *Add DAT files…* · ⋮ (*Reload database*,
  *Open folder*).
- Carte Source : radio Emulator / HTTP / Folder avec son champ (exe, URL, dossier)
  et son état (Available / Enabled / Last check / Last update).
- Table : ☐ · Active (switch) · DAT file · System · Sets · ROM entries · Version / Date · Size.
- Panneau droit **DAT information** : File name · Group · System · Sets · ROM entries ·
  Version · Date · File size · Checksum · Path · Source + *Open in folder* +
  **File preview (header)**. Pas de bouton *View file details*.
- Footer : « 17 of 17 DAT files · 29 519 sets · 214 732 ROM entries ».

**État (2026-09-12)** — `DatSource` (modèle `rom_manager.dat_groups[]` dans config.json,
migré depuis `dat_path` ; client HTTP libcurl : manifeste, refus de schéma inconnu,
comparaison **sha256** via `Glib::Checksum`, téléchargement en `.<nom>.download`
vérifié taille + sha256 puis `rename` atomique, échec = local intact + temporaire
supprimé ; `read_header`) ; `RomDatTab` branchée, ancien onglet DAT retiré, *DAT Files* /
*Generate DAT* retirés de Settings. Source primaire unique (radio) : emulator →
*Generate from FBNeo* (grisé sans exécutable), http → *Check for updates* + *Download
DATs*, folder → *Rescan folder* ; *Add DAT files…* toujours ; ⋮ *Reload database from DAT
files* (nommé, avec confirmation) + *Open folder*. Rechargement automatique (sans
confirmation) après Download / Generate / Add / Rescan / toggle Active. DAT inactifs
ignorés par `DATUpdateDialog` (jeux retirés). Panneaux DAT information / Source
information / preview d'en-tête. **Décision prise** : source par défaut d'une install
neuve = **http** vers le serveur Bootcade (le canal de distribution) ; emulator reste
disponible. **Vérifié runtime** (app complète, dossier DAT scratch, serveur réel) :
Check (2 écarts détectés : altéré + manquant), Download (2 fichiers, sha256 conformes,
aucun temporaire), rechargement auto, source folder + Rescan, DAT inactif exclu.
**À VÉRIFIER** : *Generate from FBNeo* depuis l'onglet (écrit dans le vrai
`~/.local/share/fbneo`), *Add DAT files…*, toggle Active à la souris, changement de
dossier via Browse.

---

## 7. Tests du workflow complet

1. DAT : Check for updates → Download → resync auto → compteurs cohérents.
2. Library : Scan ROMs → Audit → Missing/Incorrect/Fixable/Orphan justes sur la lib
   non-merged **et** sur le corpus split de 1a (style = split).
3. Library → *Send fixable to Import* → Import Analyse → Fix selected → Outbox.
4. Outbox : Destination affichée, Move selected → fichier remplacé retrouvé en
   Quarantine `replaced` → rescan auto → Audit again : set devenu Correct.
5. Quarantine : Restore to Import d'un `unknown` → ré-identifié par contenu.
6. Ignore this set → disparaît des problèmes, compté dans Ignored, persiste au redémarrage.
7. Corpus split chargé par FBNeo (gate de 1a).

---

## 8. Points tranchés après relecture (2026-09-11)

- Sets ignorés : **visibles**, badge + filtre *Ignored* (voir 1c).
- *Search on web* : **DuckDuckGo**, non configurable en v1.
- `dat-manifest.json` : **schéma versionné** (`schema_version`), contrat + script homelab
  figés et commités le 2026-09-12 (voir §6) ; déploiement CT105 par Gilbert, puis client.

Plus aucun point ouvert. Prochaine étape : §1a.
