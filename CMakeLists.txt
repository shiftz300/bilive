# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()
project(bilive VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus)
endif()

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Network)
add_library(bilive-core STATIC src/protocol.cpp src/network.cpp)
target_include_directories(bilive-core PUBLIC include)
target_link_libraries(bilive-core PUBLIC Qt6::Core Qt6::Network)
set_target_properties(bilive-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

include(CTest)
if(BUILD_TESTING)
    add_executable(bilive-protocol-test tests/protocol_test.cpp)
    target_link_libraries(bilive-protocol-test PRIVATE bilive-core)
    add_test(NAME bilive-protocol COMMAND bilive-protocol-test)
endif()

option(BILIVE_OBS_PLUGIN "Build the native Qt OBS plugin" OFF)
if(BILIVE_OBS_PLUGIN)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets)
    add_library(bilive MODULE src/plugin.cpp src/accounts.cpp src/automation.cpp
        src/cover.cpp src/helper.cpp src/pk.cpp third_party/qrcodegen/qrcodegen.cpp
        third_party/startlive-capture/plugin.cpp third_party/startlive-capture/media_transport.cpp
        third_party/startlive-capture/shared_video.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(bilive PRIVATE Threads::Threads)
    target_compile_definitions(bilive PRIVATE STARTLIVE_PK_MAX_FPS=30)
    if(APPLE)
        target_link_libraries(bilive PRIVATE "-framework Security")
    elseif(WIN32)
        target_link_libraries(bilive PRIVATE advapi32)
        target_compile_definitions(bilive PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    elseif(UNIX)
        target_link_libraries(bilive PRIVATE rt)
    endif()
    target_include_directories(bilive PRIVATE third_party/qrcodegen)
    target_link_libraries(bilive PRIVATE bilive-core Qt6::Widgets)
    set_target_properties(bilive PROPERTIES PREFIX "")

    if(BILIVE_OBS_SOURCE AND NOT APPLE)
        set(BILIVE_OBS_LIB "" CACHE FILEPATH "libobs shared link library")
        set(BILIVE_FRONTEND_LIB "" CACHE FILEPATH "OBS frontend shared link library")
        set(BILIVE_SIMDE_INCLUDE "" CACHE PATH "Directory containing simde/")
        if(NOT EXISTS "${BILIVE_OBS_LIB}" OR NOT EXISTS "${BILIVE_FRONTEND_LIB}")
            message(FATAL_ERROR "BILIVE_OBS_LIB and BILIVE_FRONTEND_LIB must point to the libraries for the selected OBS build")
        endif()
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obsconfig.h"
                   "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
        target_include_directories(bilive PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
            "${BILIVE_OBS_SOURCE}/libobs" "${BILIVE_OBS_SOURCE}/frontend/api" "${BILIVE_SIMDE_INCLUDE}")
        target_link_libraries(bilive PRIVATE "${BILIVE_OBS_LIB}" "${BILIVE_FRONTEND_LIB}")
    elseif(APPLE AND BILIVE_OBS_SOURCE)
        # The packaged OBS app includes libraries, while its exact tagged source
        # and SIMDe supply the headers for local development.
        set(BILIVE_OBS_APP "/Applications/OBS.app" CACHE PATH "Installed OBS app")
        set(BILIVE_SIMDE_INCLUDE "" CACHE PATH "Directory containing simde/")
        set(obs_frameworks "${BILIVE_OBS_APP}/Contents/Frameworks")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obsconfig.h"
                   "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
        target_include_directories(bilive PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
            "${BILIVE_OBS_SOURCE}/libobs" "${BILIVE_OBS_SOURCE}/frontend/api"
            "${BILIVE_SIMDE_INCLUDE}")
        # Avoid adding the installed app to the compiler's framework search
        # paths. The Qt headers must resolve from the SDK, not the app bundle.
        set(obs_link "${CMAKE_CURRENT_BINARY_DIR}/libobs-bilive-sdk.dylib")
        file(CREATE_LINK "${obs_frameworks}/libobs.framework/Versions/A/libobs" "${obs_link}" SYMBOLIC)
        target_link_libraries(bilive PRIVATE "${obs_link}" "${obs_frameworks}/libobs-frontend-api.1.dylib")
    else()
        find_package(libobs CONFIG REQUIRED)
        find_package(obs-frontend-api CONFIG REQUIRED)
        target_link_libraries(bilive PRIVATE OBS::libobs OBS::obs-frontend-api)
    endif()

    if(APPLE)
        set(bilive_resources resources/livehime.json LICENSE third_party/qrcodegen/LICENSE.txt)
        target_sources(bilive PRIVATE ${bilive_resources})
        set_source_files_properties(${bilive_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(bilive PROPERTIES
            BUNDLE TRUE BUNDLE_EXTENSION plugin
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path/../Frameworks")
        set(data_dest bilive.plugin/Contents/Resources)
        install(TARGETS bilive BUNDLE DESTINATION . LIBRARY DESTINATION .)
    else()
        # Both modern Windows and Linux OBS loaders take a per-plugin
        # directory containing the module at its root and a data/ sibling.
        set(data_dest bilive/data)
        install(TARGETS bilive RUNTIME DESTINATION bilive LIBRARY DESTINATION bilive)
    endif()

    install(FILES resources/livehime.json LICENSE DESTINATION "${data_dest}")
    install(DIRECTORY helper/ DESTINATION "${data_dest}/helper"
        PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
    set(BILIVE_HELPER_RUNTIME "" CACHE PATH "Bundled CPython 3.13 runtime")
    set(BILIVE_HELPER_SITE "" CACHE PATH "Installed platform helper dependencies")
    if(BILIVE_HELPER_RUNTIME AND BILIVE_HELPER_SITE)
        if(BUILD_TESTING)
            if(WIN32)
                set(helper_python "${BILIVE_HELPER_RUNTIME}/python.exe")
            else()
                set(helper_python "${BILIVE_HELPER_RUNTIME}/bin/python3.13")
            endif()
            add_test(NAME bilive-helper COMMAND "${helper_python}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/helper_test.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/helper/bilive_helper.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/livehime.json")
            set_tests_properties(bilive-helper PROPERTIES
                ENVIRONMENT "PYTHONPATH=${BILIVE_HELPER_SITE}"
                TIMEOUT 50)
        endif()
        install(DIRECTORY "${BILIVE_HELPER_RUNTIME}/" DESTINATION "${data_dest}/helper/runtime"
            USE_SOURCE_PERMISSIONS PATTERN "__pycache__" EXCLUDE)
        install(DIRECTORY "${BILIVE_HELPER_SITE}/" DESTINATION "${data_dest}/helper/site"
            USE_SOURCE_PERMISSIONS PATTERN "__pycache__" EXCLUDE)
    endif()
endif()
