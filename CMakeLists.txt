
cmake_minimum_required(VERSION 3.18)

project(tang_programmer LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared library
add_library(tang_programmer SHARED
    src/tang_programmer.cpp
)

target_include_directories(tang_programmer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Command-line executable
add_executable(tang_programmer_cli
    src/main.cpp
)

target_link_libraries(tang_programmer_cli
    PRIVATE
        tang_programmer
)

set_target_properties(tang_programmer_cli PROPERTIES
    OUTPUT_NAME tang_programmer
)
