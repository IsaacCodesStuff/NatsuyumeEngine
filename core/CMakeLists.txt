cmake_minimum_required(VERSION 3.22)
project(natsuyume_core VERSION 0.8.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    src/domain/track.cpp
    src/core/natsuyumecore.cpp
    src/core/lrcparser.cpp
    src/library/fileindexer.cpp
    src/library/library.cpp
    src/library/metadata.cpp
    src/library/userdata.cpp
    src/playback/playback.cpp
    src/playback/queue.cpp
    src/player/queuesession.cpp
    src/player/queuemanager.cpp
    src/player/playbackmanager.cpp
    src/player/librarymanager.cpp
    src/player/userdatamanager.cpp
)

if(ANDROID)
    # SQLite3 amalgamation — compiled directly, no system lib needed
    list(APPEND CORE_SOURCES ${JNI_INCLUDE}/sqlite3.c)
endif()

# ---------------------------------------------------------------------------
# Shared library target
# ---------------------------------------------------------------------------
add_library(natsuyume_core SHARED ${CORE_SOURCES})

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(natsuyume_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/domain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/library
    ${CMAKE_CURRENT_SOURCE_DIR}/src/playback
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
)

if(ANDROID)
    target_include_directories(natsuyume_core PRIVATE
        ${JNI_INCLUDE}            # sqlite3.h, unicode/ live here
        ${JNI_INCLUDE}/taglib
        ${JNI_INCLUDE}/mpv
    )

    # ---------------------------------------------------------------------------
    # Prebuilt libraries
    # -----------------------------------------------------------------------
    add_library(mpv SHARED IMPORTED)
    set_target_properties(mpv PROPERTIES
        IMPORTED_LOCATION ${JNI_LIBS}/libmpv.so
    )

    add_library(tag STATIC IMPORTED)
    set_target_properties(tag PROPERTIES
        IMPORTED_LOCATION ${JNI_LIBS}/libtag.a
    )

    # ---------------------------------------------------------------------------
    # Android system libraries
    # -----------------------------------------------------------------------
    find_library(log-lib log)

    # ---------------------------------------------------------------------------
    # Link
    # -----------------------------------------------------------------------
    set(CMAKE_ANDROID_STL_TYPE c++_static)
    target_link_libraries(natsuyume_core PRIVATE
        mpv
        tag
        icu          # NDK-provided ICU stub (replaces icuuc on Android)
        ${log-lib}
    )
else()
    # -----------------------------------------------------------------------
    # Desktop Linux dependencies via pkg-config
    # -----------------------------------------------------------------------
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
    pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
    pkg_check_modules(ICU REQUIRED IMPORTED_TARGET icu-uc icu-i18n)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

    target_link_libraries(natsuyume_core PRIVATE
        PkgConfig::MPV
        PkgConfig::TAGLIB
        PkgConfig::ICU
        PkgConfig::SQLITE3
    )

    # TagLib pkg-config points to <prefix>/include/taglib, but sources use <taglib/*.h>
    foreach(_taglib_inc ${TAGLIB_INCLUDE_DIRS})
        get_filename_component(_taglib_parent ${_taglib_inc} DIRECTORY)
        target_include_directories(natsuyume_core PRIVATE ${_taglib_parent})
    endforeach()
endif()
