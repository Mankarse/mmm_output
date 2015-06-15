cmake_minimum_required(VERSION 2.8)

project(mmm_output_kdevelop)
SET (CMAKE_C_COMPILER             "clang-3.5")
add_compile_options(-std=gnu99)#Need to use gnu C on linux, since some system headers require it to work correctly (e.g. ALSA)
add_compile_options(-Wall -Wextra -pedantic -Werror)

SET(CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES "$ENV{HOME}/raspberrypi/rootfs/include")

find_path(ALSA_INCLUDE_DIRS alsa/asoundlib.h DOC "Include for ALSA")
find_library(ALSA_LIBRARY asound DOC "ALSA library file")
find_path(SNDFILE_INCLUDE_DIRS sndfile.h DOC "Include for libsndfile")
find_library(SNDFILE_LIBRARY sndfile DOC "libsndfile library file")
find_path(SYSTEMD_DAEMON_INCLUDE_DIRS systemd/sd-daemon.h DOC "Include for libsndfile")
find_library(SYSTEMD_DAEMON_LIBRARY systemd-daemon DOC "libsndfile library file")

add_executable(mmm_output ../mmm_output/command_parser.c ../mmm_output/main.c ../mmm_output/playlist_player.c)

target_include_directories(mmm_output PRIVATE ${ALSA_INCLUDE_DIRS})
target_include_directories(mmm_output PRIVATE ${SNDFILE_INCLUDE_DIRS})
target_include_directories(mmm_output PRIVATE ${SYSTEMD_DAEMON_INCLUDE_DIRS})

target_link_libraries(mmm_output m)
target_link_libraries(mmm_output ${ALSA_LIBRARY})
target_link_libraries(mmm_output ${SNDFILE_LIBRARY})
target_link_libraries(mmm_output ${SYSTEMD_DAEMON_LIBRARY})

install(TARGETS mmm_output RUNTIME DESTINATION bin)
