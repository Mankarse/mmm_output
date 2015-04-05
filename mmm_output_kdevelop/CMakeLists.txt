cmake_minimum_required(VERSION 2.8)

project(mmm_output_kdevelop)
add_compile_options(-std=gnu99)
add_compile_options(-Wall -Wextra -pedantic)
add_executable(mmm_output ../mmm_output/main.c)

target_link_libraries(mmm_output asound)
target_link_libraries(mmm_output m)
target_link_libraries(mmm_output sndfile)

install(TARGETS mmm_output RUNTIME DESTINATION bin)
