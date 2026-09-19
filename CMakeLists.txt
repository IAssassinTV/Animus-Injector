cmake_minimum_required(VERSION 3.20)
project(animus_injector LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(SOURCES
    src/dllmain.cpp
    src/config.cpp
    src/fixes.cpp
    src/hooks.cpp
    src/logger.cpp
    external/safetyhook/safetyhook.cpp
    external/safetyhook/Zydis.c
)

add_library(animus_injector SHARED ${SOURCES})

set_target_properties(animus_injector PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "AnimusInjector"
)

target_include_directories(animus_injector PRIVATE
    src
    external/inipp
    external/spdlog/include
    external/safetyhook
)

target_compile_definitions(animus_injector PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    ZYDIS_STATIC_BUILD
    ZYCORE_STATIC_BUILD
)

target_link_libraries(animus_injector PRIVATE
    ws2_32
    psapi
    xinput
)

if(MSVC)
    target_compile_options(animus_injector PRIVATE /utf-8 /EHsc)
    set_property(TARGET animus_injector PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_link_options(animus_injector PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

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