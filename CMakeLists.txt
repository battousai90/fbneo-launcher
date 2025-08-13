cmake_minimum_required(VERSION 3.10)
project(fbneo-launcher)
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
# --- ZLIB ---
pkg_check_modules(ZLIB REQUIRED zlib)
# --- Threads ---
find_package(Threads REQUIRED)
# Ensure that the pthreads library is found
add_executable(fbneo-launcher
    src/main.cpp
    src/MainWindow.cpp
    src/GameRow.cpp
    src/SettingsPanel.cpp
    src/DatParser.cpp
    src/RomScanner.cpp
    src/ScanCache.cpp
    src/AppContext.cpp
    src/IconManager.cpp
)
# --- Source files ---
target_link_libraries(fbneo-launcher
    ${GTKMM_LIBRARIES}
    ${PUGIXML_LIBRARIES}
    ${ZIP_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Threads::Threads  # pthreads
)
# --- Include directories ---
target_include_directories(fbneo-launcher PRIVATE
    ${GTKMM_INCLUDE_DIRS}
    ${PUGIXML_INCLUDE_DIRS}
    ${ZIP_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
)
# --- Compiler options ---
target_compile_options(fbneo-launcher PRIVATE
    ${GTKMM_CFLAGS_OTHER}
    ${PUGIXML_CFLAGS_OTHER}
    ${ZIP_CFLAGS_OTHER}
    ${ZLIB_CFLAGS_OTHER}
)


set_target_properties(fbneo-launcher PROPERTIES
    INSTALL_RPATH "$ORIGIN/lib:$ORIGIN/lib64"
)

# --- Install rules ---
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/assets"
    DEPENDS fbneo-launcher
    COMMENT "Copying assets to build directory"
)