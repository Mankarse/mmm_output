cmake_minimum_required(VERSION 2.8)

project(mmm_output_kdevelop)
SET(CMAKE_C_COMPILER "clang-3.5")
add_compile_options(-std=c11)
add_compile_options(-Wall -Wextra -Weverything -Wno-padded -pedantic)#with clang, add --analyze for static analysis.

#See http://clang.llvm.org/docs/UsersManual.html#controlling-code-generation
#There are mutually incompatible options, chose wisely.
#Also, some options are incompatible with Valgrind, and some 
#options require that all referenced libraries also be compiled with that option.
add_compile_options(-fsanitize=undefined)
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")

add_definitions(-D_POSIX_C_SOURCE=200809L -D_FORTIFY_SOURCE=2) #We need some posix libraries to compile properly on linux. Might not be needed elsewhere.
#add_compile_options(-Os)
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
