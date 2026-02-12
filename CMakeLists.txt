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
        options.cpp
        el/fd.hpp
        el/future.hpp
        el/intrusive_list.hpp
        el/fd_registrant.hpp
        el/fd_registrant.cpp)

target_include_directories(tpf PRIVATE .)
