cmake_minimum_required(VERSION 3.22.1)
project(klencod_pty)

add_library(klencod_pty SHARED native_pty.cpp)
target_compile_features(klencod_pty PRIVATE cxx_std_17)
target_compile_options(klencod_pty PRIVATE -Wall -Wextra)
target_link_libraries(klencod_pty log)
