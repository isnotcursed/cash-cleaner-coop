cmake_minimum_required(VERSION 3.16)
project(cash_cleaner_coop LANGUAGES CXX)

enable_testing()
add_subdirectory(protocol)

option(CCSCOOP_BUILD_UE4SS_MOD "Build the UE4SS C++ bootstrap mod" OFF)
set(CCSCOOP_UE4SS_SOURCE_DIR "" CACHE PATH "Path to the matching RE-UE4SS source tree")

if(CCSCOOP_BUILD_UE4SS_MOD)
    if(NOT CCSCOOP_UE4SS_SOURCE_DIR)
        message(FATAL_ERROR "CCSCOOP_UE4SS_SOURCE_DIR is required when building the UE4SS mod")
    endif()

    add_subdirectory("${CCSCOOP_UE4SS_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/re-ue4ss")
    add_subdirectory(cppmod)
endif()
