cmake_minimum_required(VERSION 4.0)
project(tpf)

set(CMAKE_CXX_STANDARD 23)

add_executable(tpf
        main.cpp
        util.hpp
        el/loop.hpp
        el/loop.cpp
        el/pipe.cpp
        el/pipe.hpp
        options.hpp
        options.cpp)

target_include_directories(tpf PRIVATE .)
