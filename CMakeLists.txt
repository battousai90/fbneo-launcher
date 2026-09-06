cmake_minimum_required(VERSION 3.16)

# The number the release CI bumps, and the only version a source tarball with
# no git metadata can know. Packaging (CPack, below) uses it as is: a package
# filename wants a clean release number, never a commit hash.
project(fbneo-launcher VERSION 1.0.21 LANGUAGES CXX)

# --- Version the binary reports about itself -----------------------------
# Derived from git whenever there is a git tree, and NOT from the literal
# above. A literal that only the CI updates makes every build from a checkout
# that is even one commit behind claim the previous release: it then shows
# itself an update banner for a version it may already contain, which is
# exactly how "Bootcade 1.0.18 is available, you are running 1.0.17" appeared
# on a Dev build sitting past that very release.
#
# Exactly at a tag this yields the bare release number, identical to the
# literal, so releases are unaffected. Anywhere else it yields something
# visibly unreleased ("1.2.3+7.g1a2b3c4", with ".dirty" appended when the tree
# has local edits), which the update check reads as equal to 1.2.3 : std::stoi
# stops at the "+", so no banner nags a dev build that is level with the
# release, while a genuinely older one still gets told.
#
# Resolved at configure time, so the commit part refreshes when CMake re-runs
# rather than on every compile. The X.Y.Z part, the only piece the update
# check compares, changes solely at a release : which edits this file and
# re-runs CMake anyway.
set(FBNEO_VERSION_STRING "${PROJECT_VERSION}")
execute_process(
    COMMAND git describe --tags --match "v[0-9]*" --dirty
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(GIT_DESCRIBE)
    string(REGEX REPLACE "^v" "" GIT_DESCRIBE "${GIT_DESCRIBE}")
    string(REGEX REPLACE "-([0-9]+)-g([0-9a-f]+)" "+\\1.g\\2"
           FBNEO_VERSION_STRING "${GIT_DESCRIBE}")
    string(REGEX REPLACE "-dirty$" ".dirty"
           FBNEO_VERSION_STRING "${FBNEO_VERSION_STRING}")
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
    src/DatParser.cpp
    src/RomScanner.cpp
    src/RomArchive.cpp
    src/RomInbox.cpp
    src/RomAudit.cpp
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