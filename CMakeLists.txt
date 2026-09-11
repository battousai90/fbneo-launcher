cmake_minimum_required(VERSION 3.16)

# LA VERSION SE DECLARE ICI, AU DEBUT DU TRAVAIL, PAS A LA PUBLICATION.
#
# Avant d'ecrire la premiere ligne d'une modification, on decide ce qu'elle est
# au sens de SemVer et on bumpe ce numero en consequence :
#
#   PATCH  (1.0.21 -> 1.0.22)  correction de bug, aucun comportement nouveau
#   MINOR  (1.0.21 -> 1.1.0)   fonctionnalite nouvelle, retrocompatible
#   MAJOR  (1.0.21 -> 2.0.0)   rupture de compatibilite
#
# Le binaire annonce alors ce numero suivi des metadonnees de build :
# "1.1.0+4.g3a86c7f.dirty". Tout ce qui suit le "+" est de la metadonnee au
# sens de SemVer, donc separee par des points : ".dirty" et non "-dirty", un
# tiret introduisant une PRE-VERSION, qui devrait se placer avant le "+".
#
# C'est ce numero que la publication reprend tel quel : le CI ne devine plus la
# version, il publie celle qui est declaree ici (voir
# .github/workflows/release.yml). Deduire la version a la publication ne pouvait
# de toute facon produire que des PATCH : une machine ne peut pas savoir qu'une
# livraison apporte des fonctionnalites.
#
# Packaging (CPack, plus bas) l'utilise tel quel : un nom de paquet veut un
# numero de release propre, jamais une empreinte de commit.
project(fbneo-launcher VERSION 1.1.0 LANGUAGES CXX)

# --- Ce que le binaire annonce -------------------------------------------
#
# La partie X.Y.Z vient du numero declare ci-dessus, et de nulle part ailleurs.
# S'y ajoutent les metadonnees de build tant qu'on n'est pas exactement sur le
# tag correspondant :
#
#   sur le tag v1.1.0        -> "1.1.0"                    (build de release)
#   4 commits plus loin      -> "1.1.0+4.g3a86c7f"
#   avec des modifs en cours -> "1.1.0+4.g3a86c7f.dirty"
#
# Le "+" est donc la marque d'un build non publie : la verification de mise a
# jour s'en sert pour ne jamais proposer une release a une machine de
# developpement (voir MainWindow::check_app_update_async).
#
# Le compte de commits part du dernier TAG EXISTANT, jamais de "git describe"
# seul : describe remonte au plus proche ancetre tague, or le CI de release
# commite le bump sur main puis le redescend sur Dev. Un clone qui n'a pas
# encore recupere ce commit n'a pas le tag dans ses ancetres, et describe
# repartait alors de l'avant-derniere version.
#
# Resolu au moment de la configuration, donc rafraichi quand CMake est relance.
set(FBNEO_VERSION_STRING "${PROJECT_VERSION}")

execute_process(
    COMMAND git describe --tags --exact-match --match "v${PROJECT_VERSION}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE FBNEO_ON_RELEASE_TAG)

if(NOT FBNEO_ON_RELEASE_TAG EQUAL 0)
    execute_process(
        COMMAND git tag --list "v[0-9]*"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE FBNEO_TAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(FBNEO_TAGS)
        # Tri NATUREL, sans quoi v1.0.9 passerait apres v1.0.21.
        string(REPLACE "\n" ";" FBNEO_TAGS "${FBNEO_TAGS}")
        list(SORT FBNEO_TAGS COMPARE NATURAL)
        list(GET FBNEO_TAGS -1 FBNEO_LATEST_TAG)

        # ── Le garde-fou ────────────────────────────────────────────────
        #
        # Un build de developpement ne peut PAS porter un numero inferieur ou
        # egal a la derniere version publiee : ce serait annoncer aux
        # utilisateurs un logiciel plus vieux que celui qu'ils ont installe, et
        # c'est le defaut qui est revenu a chaque release.
        #
        # Il ne revient plus, parce que la configuration s'arrete ici. Oublier
        # de bumper apres une publication n'est plus quelque chose qu'on
        # decouvre trois jours plus tard dans les reglages : ca ne compile pas.
        string(REGEX REPLACE "^v" "" FBNEO_LATEST_VERSION "${FBNEO_LATEST_TAG}")
        if(NOT PROJECT_VERSION VERSION_GREATER FBNEO_LATEST_VERSION)
            message(FATAL_ERROR
                "Version declaree en retard : CMakeLists.txt annonce "
                "${PROJECT_VERSION}, or ${FBNEO_LATEST_VERSION} est deja "
                "publiee.\n"
                "Bumper 'project(fbneo-launcher VERSION ...)' AVANT de coder, "
                "selon SemVer :\n"
                "  correction de bug          -> PATCH\n"
                "  fonctionnalite nouvelle    -> MINOR (remet le patch a 0)\n"
                "  rupture de compatibilite   -> MAJOR\n"
                "Si le tag est simplement absent en local : git fetch --tags.")
        endif()

        execute_process(
            COMMAND git rev-list --count "${FBNEO_LATEST_TAG}..HEAD"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE FBNEO_BUILD_NUM
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE FBNEO_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(FBNEO_BUILD_NUM AND FBNEO_SHA)
        string(APPEND FBNEO_VERSION_STRING "+${FBNEO_BUILD_NUM}.g${FBNEO_SHA}")
    endif()

    # Des modifications non commitees : le binaire ne correspond a aucun
    # commit, et doit le dire. --untracked-files=no expres : un fichier jamais
    # ajoute (une note, un journal) ne change pas ce qui est compile.
    execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE FBNEO_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(FBNEO_DIRTY)
        string(APPEND FBNEO_VERSION_STRING ".dirty")
    endif()
endif()
message(STATUS "fbneo-launcher version: ${FBNEO_VERSION_STRING}")

set(APP_ID "io.github.battousai90.FbneoLauncher")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# --- Find packages ---
# Ensure that the required packages are found and linked correctly  
find_package(PkgConfig REQUIRED)
# --- GTKMM ---
pkg_check_modules(GTKMM REQUIRED gtkmm-3.0)
# --- PUGIXML ---
pkg_check_modules(PUGIXML REQUIRED pugixml)
# --- ZIP ---
pkg_check_modules(ZIP REQUIRED libzip)
# --- LIBARCHIVE (7z / rar / tar reading for the ROM manager inbox) ---
pkg_check_modules(ARCHIVE REQUIRED libarchive)
# --- ZLIB ---
pkg_check_modules(ZLIB REQUIRED zlib)
# --- CURL ---
pkg_check_modules(CURL REQUIRED libcurl)
# --- OPENSSL (libcrypto) ---
# Uniquement pour PKCE : SHA-256 du verifieur, et tirage aleatoire sur.
# rand() ne convient pas : un verifieur previsible ne protege de rien, et
# c'est justement ce que PKCE existe pour empecher.
pkg_check_modules(CRYPTO REQUIRED libcrypto)
# --- SQLite3 ---
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
# --- Threads ---
find_package(Threads REQUIRED)

# libzip renamed zip_source_zip() to zip_source_zip_file() (with an extra password
# argument) somewhere around 1.10. Rather than guess which distribution ships
# which version, probe for the symbol itself.
include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES  ${ZIP_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${ZIP_LIBRARIES})
check_symbol_exists(zip_source_zip_file "zip.h" HAVE_ZIP_SOURCE_ZIP_FILE)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
# Ensure that the pthreads library is found
add_executable(fbneo-launcher
    src/main.cpp
    src/MainWindow.cpp
    src/GameRow.cpp
    src/SettingsPanel.cpp
    src/SettingsUi.cpp
    src/DatParser.cpp
    src/RomScanner.cpp
    src/RomArchive.cpp
    src/RomInbox.cpp
    src/RomAudit.cpp
    src/RomResolve.cpp
    src/RomManagerWindow.cpp
    src/AppContext.cpp
    src/IconManager.cpp
    src/ROMScanDialog.cpp
    src/DownloadDialog.cpp
    src/GenerateDAT.cpp
    src/FbneoUpdateCheck.cpp
    src/HiscoreClient.cpp
    src/BootcadeAuth.cpp
    src/LoginDialog.cpp
    src/ScreenshotAssignDialog.cpp
    src/SystemPrefix.cpp
    src/RomCleanup.cpp
    src/SplashScreen.cpp
    src/ThumbnailDownloader.cpp
    src/DatabaseManager.cpp
    src/DATUpdateDialog.cpp
    src/ConfirmationDialog.cpp
    src/FilterCache.cpp
    src/ControllerManager.cpp
    src/ControllerDialog.cpp
    src/i18n.cpp
)
# --- Source files ---
target_link_libraries(fbneo-launcher
    ${GTKMM_LIBRARIES}
    ${PUGIXML_LIBRARIES}
    ${ZIP_LIBRARIES}
    ${ARCHIVE_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CRYPTO_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    Threads::Threads  # pthreads
)
# --- Include directories ---
target_include_directories(fbneo-launcher PRIVATE
    ${GTKMM_INCLUDE_DIRS}
    ${PUGIXML_INCLUDE_DIRS}
    ${ZIP_INCLUDE_DIRS}
    ${ARCHIVE_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${CRYPTO_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)
# --- Compiler options ---
target_compile_options(fbneo-launcher PRIVATE
    ${GTKMM_CFLAGS_OTHER}
    ${PUGIXML_CFLAGS_OTHER}
    ${ZIP_CFLAGS_OTHER}
    ${ZLIB_CFLAGS_OTHER}
    ${CURL_CFLAGS_OTHER}
    ${SQLITE3_CFLAGS_OTHER}
)


set_target_properties(fbneo-launcher PROPERTIES
    INSTALL_RPATH "$ORIGIN/lib:$ORIGIN/lib64"
)

# Baked-in fallback for the packaged layout. AppContext probes several locations
# at runtime; this is the one an FHS install ends up using.
target_compile_definitions(fbneo-launcher PRIVATE
    FBNEO_DATA_DIR="${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME}"
    FBNEO_VERSION="${FBNEO_VERSION_STRING}"
    $<$<BOOL:${HAVE_ZIP_SOURCE_ZIP_FILE}>:HAVE_ZIP_SOURCE_ZIP_FILE>
)

# Keep the build tree runnable straight from build/ (./run_debug.sh).
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/locale" "${CMAKE_BINARY_DIR}/locale"
    DEPENDS fbneo-launcher
    COMMENT "Copying assets and translations to build directory"
)

# ─────────────────────────────────────────────────────────────────────────────
# Install rules : <prefix>/bin for the binary, <prefix>/share/fbneo-launcher for
# everything it reads at runtime, plus the freedesktop metadata packages need.
# ─────────────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS fbneo-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY assets locale
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})

install(FILES packaging/${APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES packaging/${APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

install(FILES assets/logo.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME ${APP_ID}.svg)

# ─────────────────────────────────────────────────────────────────────────────
# CPack : produces the .deb and the .tar.gz. The AppImage and the Flatpak are
# built by scripts/package.sh, which consumes this same install tree.
# ─────────────────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME              "fbneo-launcher")
set(CPACK_PACKAGE_VENDOR            "battousai90")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Native GTK launcher and ROM manager for FinalBurn Neo")
set(CPACK_PACKAGE_HOMEPAGE_URL      "https://github.com/battousai90/fbneo-launcher")
set(CPACK_PACKAGE_CONTACT           "battousai90")
set(CPACK_PACKAGE_FILE_NAME
    "fbneo-launcher-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES               ON)
set(CPACK_GENERATOR                 "DEB;TGZ")

set(CPACK_DEBIAN_PACKAGE_SECTION    "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")
set(CPACK_DEBIAN_FILE_NAME          DEB-DEFAULT)
# Let dpkg-shlibdeps work out the exact library versions from the linked binary
# rather than hard-coding a list that drifts with every distribution release.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)

include(CPack)