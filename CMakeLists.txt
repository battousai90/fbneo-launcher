cmake_minimum_required(VERSION 3.10)
project(fbneo-launcher)

set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-3.0 gdkmm-3.0)

# 🔴 AVANT : seul main.cpp était inclus
# add_executable(fbneo-launcher src/main.cpp)

# ✅ MAINTENANT : on ajoute aussi MainWindow.cpp
add_executable(fbneo-launcher
    src/main.cpp
    src/MainWindow.cpp
    src/GameRow.cpp
)

target_link_libraries(fbneo-launcher ${GTKMM_LIBRARIES})
target_include_directories(fbneo-launcher PRIVATE ${GTKMM_INCLUDE_DIRS})
target_compile_options(fbneo-launcher PRIVATE ${GTKMM_CFLAGS_OTHER})