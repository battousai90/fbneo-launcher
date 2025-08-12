cmake_minimum_required(VERSION 3.10)
project(fbneo-launcher)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# --- Find packages ---
# Ensure that the required packages are found and linked correctly  
find_package(PkgConfig REQUIRED)
# --- GTKMM ---
# Ensure that gtkmm-3.0 is found
# This is the C++ wrapper for GTK+ 3
# It provides a convenient way to create GUI applications in C++
# pkg_check_modules is used to find the gtkmm-3.0 package
# It sets the necessary variables for include directories and libraries
# GTKMM_INCLUDE_DIRS will contain the include directories for gtkmm-3.0
# GTKMM_LIBRARIES will contain the libraries needed to link against gtkmm-3.0
# GTKMM_CFLAGS_OTHER will contain any additional compiler flags needed for gtkmm-3.0
# GTKMM_VERSION will contain the version of gtkmm-3.0 found
# If gtkmm-3.0 is not found, it will raise an error
# This is important for ensuring that the application can use the GTK+ 3 features
# and that the necessary libraries are linked correctly
# It is a common practice to use pkg_check_modules to find and configure dependencies in CMake
# This allows for better portability and easier management of dependencies
# It also ensures that the application can be built on different systems without manual configuration
# If gtkmm-3.0 is not found, it will raise an error
pkg_check_modules(GTKMM REQUIRED gtkmm-3.0)
# --- PUGIXML ---
# Pugixml is a C++ XML processing library
# It is used for parsing and manipulating XML documents
# pkg_check_modules is used to find the pugixml package
# It sets the necessary variables for include directories and libraries
# PUGIXML_INCLUDE_DIRS will contain the include directories for pugixml
# PUGIXML_LIBRARIES will contain the libraries needed to link against pugixml
# PUGIXML_CFLAGS_OTHER will contain any additional compiler flags needed for pugixml
pkg_check_modules(PUGIXML REQUIRED pugixml)
# --- ZIP ---
# libzip is a C library for reading, creating, and modifying zip archives
# It provides a simple API for working with zip files
# pkg_check_modules is used to find the libzip package
# It sets the necessary variables for include directories and libraries
# ZIP_INCLUDE_DIRS will contain the include directories for libzip
# ZIP_LIBRARIES will contain the libraries needed to link against libzip
# ZIP_CFLAGS_OTHER will contain any additional compiler flags needed for libzip
pkg_check_modules(ZIP REQUIRED libzip)
# --- ZLIB ---
# Zlib is a compression library that provides functions for compressing and decompressing data
# It is often used in conjunction with libzip for handling zip files
# pkg_check_modules is used to find the zlib package
# It sets the necessary variables for include directories and libraries
# ZLIB_INCLUDE_DIRS will contain the include directories for zlib
# ZLIB_LIBRARIES will contain the libraries needed to link against zlib
# ZLIB_CFLAGS_OTHER will contain any additional compiler flags needed for zlib
pkg_check_modules(ZLIB REQUIRED zlib)
# --- Threads ---
# Threads is a CMake module that provides support for multithreading
# It is used to ensure that the pthreads library is found and linked correctly
# This is important for applications that require multithreading capabilities
# find_package(Threads REQUIRED) is used to find the pthreads library
# It sets the necessary variables for linking against pthreads
# Threads::Threads will contain the pthreads library
# This is important for ensuring that the application can use multithreading features
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