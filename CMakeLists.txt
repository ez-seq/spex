cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" SPEX_VERSION_FILE)
string(REGEX MATCH "MAJOR[ \t]*=[ \t]*([0-9]+)" _major_match "${SPEX_VERSION_FILE}")
set(SPEX_VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "MINOR[ \t]*=[ \t]*([0-9]+)" _minor_match "${SPEX_VERSION_FILE}")
set(SPEX_VERSION_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "PATCH[ \t]*=[ \t]*([0-9]+)" _patch_match "${SPEX_VERSION_FILE}")
set(SPEX_VERSION_PATCH "${CMAKE_MATCH_1}")

if(SPEX_VERSION_MAJOR STREQUAL "" OR SPEX_VERSION_MINOR STREQUAL "" OR SPEX_VERSION_PATCH STREQUAL "")
    message(FATAL_ERROR "Failed to parse MAJOR/MINOR/PATCH from VERSION.txt")
endif()

set(SPEX_SEMVER "${SPEX_VERSION_MAJOR}.${SPEX_VERSION_MINOR}.${SPEX_VERSION_PATCH}")

project(spex VERSION ${SPEX_SEMVER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.2
)
FetchContent_MakeAvailable(juce)

juce_add_gui_app(spex
    PRODUCT_NAME "spex"
)

target_sources(spex PRIVATE
    main.cpp
    MainComponent.cpp
)

target_compile_definitions(spex PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

find_package(Git QUIET)
set(SPEX_GIT_HASH "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SPEX_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(SPEX_GIT_HASH STREQUAL "")
        set(SPEX_GIT_HASH "unknown")
    endif()
endif()

target_compile_definitions(spex PRIVATE
    SPEX_APP_VERSION="${SPEX_SEMVER}"
    SPEX_GIT_HASH="${SPEX_GIT_HASH}"
)

target_compile_options(spex PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>
    $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O2>
)

target_link_libraries(spex PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
)
