cmake_minimum_required(VERSION 3.22.1)
project(main)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_library(main SHARED
        main.cpp
)

target_link_libraries(
        main
        PRIVATE
        android
        log
)
if (OXIDE_NATIVE_DIAGNOSTIC)
    target_compile_definitions(main PRIVATE OXIDE_NATIVE_DIAGNOSTIC=1)
endif()

if (OXIDE_NO_FUSION_CONTROL)
    target_compile_definitions(main PRIVATE OXIDE_NO_FUSION_CONTROL=1)
endif()
