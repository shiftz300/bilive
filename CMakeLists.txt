# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()
project(bilive VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    add_library(bilive MODULE src/plugin.cpp third_party/qrcodegen/qrcodegen.cpp)
    target_include_directories(bilive PRIVATE third_party/qrcodegen)
    target_link_libraries(bilive PRIVATE bilive-core Qt6::Widgets)
    set_target_properties(bilive PROPERTIES PREFIX "")
    if(MSVC)
        target_compile_options(bilive PRIVATE /utf-8)
        target_compile_options(bilive-core PRIVATE /utf-8)
        if(TARGET bilive-protocol-test)
            target_compile_options(bilive-protocol-test PRIVATE /utf-8)
        endif()
    endif()

    if(APPLE AND BILIVE_OBS_SOURCE)
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
        install(TARGETS bilive BUNDLE DESTINATION .)
    elseif(WIN32)
        install(TARGETS bilive RUNTIME DESTINATION obs-plugins/64bit
                               LIBRARY DESTINATION obs-plugins/64bit)
        install(FILES resources/livehime.json DESTINATION data/obs-plugins/bilive)
    else()
        include(GNUInstallDirs)
        install(TARGETS bilive LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
        install(FILES resources/livehime.json DESTINATION "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/bilive")
    endif()
endif()
