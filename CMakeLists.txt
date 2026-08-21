cmake_minimum_required(VERSION 3.16)
project(fbneo-launcher VERSION 1.0.8 LANGUAGES CXX)
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
    src/ScreenshotAssignDialog.cpp
    src/SystemPrefix.cpp
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
    FBNEO_VERSION="${PROJECT_VERSION}"
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
# Install rules — <prefix>/bin for the binary, <prefix>/share/fbneo-launcher for
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
# CPack — produces the .deb and the .tar.gz. The AppImage and the Flatpak are
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