cmake_minimum_required(VERSION 3.22)

# Both must be set before project() to take effect.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(AdormisCdjExport VERSION 0.1.8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# JUCE 9.0.4, pinned by commit and checksum: the same JUCE as Adormis Audio,
# so cdj_core can later be compiled into it unchanged.
# For local builds, -DCDJ_JUCE_DIR=/path/to/JUCE skips the download.
set(CDJ_JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional)")

if(NOT TARGET juce::juce_core)
    if(CDJ_JUCE_DIR)
        add_subdirectory(${CDJ_JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(JUCE
            URL      https://github.com/juce-framework/JUCE/archive/3fb5cbc0340758596dac39f7c8de47d562b4896e.tar.gz
            URL_HASH SHA256=2bb7095497a7bb7269dbdf841a7c6aa0939eaeb2a2bc9724ac4758d3ae3565a7)
        FetchContent_MakeAvailable(JUCE)
    endif()
endif()

# cdj_core: reading tracks and markers, cues, drives, and writing the stick
# (audio, rekordbox XML and the Pioneer library: export.pdb, ANLZ, settings).
# No GUI, so Adormis Audio can link it as it is. INTERFACE library: every
# target compiles the sources with its own JUCE modules.
add_library(cdj_core INTERFACE)
target_sources(cdj_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Track.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/CuePlan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Tempo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Drives.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Format.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Library.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Mp3.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/RekordboxXml.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Exporter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/PioneerPdb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/PioneerAnlz.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/PioneerSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/PioneerWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Serato.cpp)
target_include_directories(cdj_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(cdj_core INTERFACE juce::juce_audio_formats juce::juce_dsp)
target_compile_definitions(cdj_core INTERFACE CDJ_VERSION="${PROJECT_VERSION}" JUCE_USE_MP3AUDIOFORMAT=1)

# MP3 encoding through LAME (libmp3lame, LGPL), when it's installed.
find_path(LAME_INCLUDE_DIR lame/lame.h)
find_library(LAME_LIBRARY NAMES libmp3lame.a mp3lame libmp3lame)

if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    message(STATUS "MP3 encoding enabled: ${LAME_LIBRARY}")
    target_include_directories(cdj_core INTERFACE ${LAME_INCLUDE_DIR})
    target_link_libraries(cdj_core INTERFACE ${LAME_LIBRARY})
    target_compile_definitions(cdj_core INTERFACE CDJ_HAS_MP3=1)
else()
    message(STATUS "MP3 encoding disabled: libmp3lame not found")
    target_compile_definitions(cdj_core INTERFACE CDJ_HAS_MP3=0)
endif()

# A parent project (Adormis Audio) only gets cdj_core and cdj_ui.
add_library(cdj_ui INTERFACE)
target_sources(cdj_ui INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/app/ExportComponent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app/TrackEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app/FormatDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app/AudioBrowser.cpp)
target_include_directories(cdj_ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(cdj_ui INTERFACE cdj_core juce::juce_gui_extra juce::juce_audio_utils)

if(NOT PROJECT_IS_TOP_LEVEL)
    return()
endif()

juce_add_gui_app(AdormisCdjExport
    PRODUCT_NAME "Adormis CDJ Export"
    COMPANY_NAME "Adormis"
    BUNDLE_ID io.github.jastincheis.adormiscdjexport)

target_sources(AdormisCdjExport PRIVATE app/Main.cpp)

target_compile_definitions(AdormisCdjExport PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AdormisCdjExport,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AdormisCdjExport,JUCE_VERSION>")

target_link_libraries(AdormisCdjExport PRIVATE
    cdj_ui
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Tests: build real sticks in a temporary folder and check them.
juce_add_console_app(cdj_tests PRODUCT_NAME "cdj_tests")
target_sources(cdj_tests PRIVATE tests/Tests.cpp)
target_compile_definitions(cdj_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(cdj_tests PRIVATE cdj_core juce::juce_recommended_config_flags)
