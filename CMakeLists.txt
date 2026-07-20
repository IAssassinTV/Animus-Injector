cmake_minimum_required(VERSION 3.20)
project(claudia LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(SOURCES
    src/dllmain.cpp
    src/config.cpp
    src/fixes.cpp
    src/gui.cpp
    src/hooks.cpp
    src/launcher.cpp
    src/logger.cpp
    external/safetyhook/safetyhook.cpp
    external/safetyhook/Zydis.c
)

add_library(claudia SHARED ${SOURCES})

set_target_properties(claudia PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "claudia"
)

target_include_directories(claudia PRIVATE
    src
    external/inipp
    external/spdlog/include
    external/safetyhook
)

target_compile_definitions(claudia PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    ZYDIS_STATIC_BUILD
    ZYCORE_STATIC_BUILD
)

target_link_libraries(claudia PRIVATE
    user32
    ws2_32
    comctl32
    psapi
    xinput
)

if(MSVC)
    target_compile_options(claudia PRIVATE /utf-8 /EHsc)
    set_property(TARGET claudia PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_link_options(claudia PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

add_custom_command(TARGET claudia POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/claudia.ini
        $<TARGET_FILE_DIR:claudia>/claudia.ini
)

set(LOADER_SOURCES
    src/loader/dllmain.cpp
    src/loader/exports.cpp
)

add_library(loader SHARED ${LOADER_SOURCES} src/loader/exports.def)

set_target_properties(loader PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dinput8"
)

target_include_directories(loader PRIVATE
    src
)

target_compile_definitions(loader PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    DIRECTINPUT_VERSION=0x0800
)

target_link_libraries(loader PRIVATE
    dinput8
)

if(MSVC)
    target_compile_options(loader PRIVATE /utf-8 /EHsc)
    set_property(TARGET loader PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_link_options(loader PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()
