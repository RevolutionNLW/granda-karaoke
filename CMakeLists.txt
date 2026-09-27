cmake_minimum_required(VERSION 3.22)

project(FrankiesKaraokeStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Sql)
include(cmake/GStreamer.cmake)

# Development runs link against the SDK in place. Packaging/deployment of the
# GStreamer runtime is a later milestone.
if(GSTREAMER_LIBRARY_DIR)
    set(CMAKE_BUILD_RPATH "${GSTREAMER_LIBRARY_DIR}")
endif()

# CDG decoding: plain C++ with no Qt or GStreamer dependency, so it can be
# tested in isolation.
add_library(fks_cdg STATIC
    src/cdg/CdgDecoder.cpp
    src/cdg/CdgDecoder.h
    src/cdg/CdgTitleFrames.cpp
    src/cdg/CdgTitleFrames.h
)
target_include_directories(fks_cdg PUBLIC src)

# Read-only song-library parsing primitives.  This target deliberately has no
# UI or media-playback dependency so the parsers remain independently testable.
add_library(fks_library STATIC
    src/library/Catalogue.cpp
    src/library/Catalogue.h
    src/library/CatalogueTools.cpp
    src/library/CatalogueTools.h
    src/library/ContentIdentity.cpp
    src/library/ContentIdentity.h
    src/library/KaraokeLabels.cpp
    src/library/KnownLibraryRoots.cpp
    src/library/UserStateStore.cpp
    src/library/KaraokeLabels.h
    src/library/FilenameParser.cpp
    src/library/FilenameParser.h
    src/library/Id3Reader.cpp
    src/library/Id3Reader.h
    src/library/LibraryScanner.cpp
    src/library/LibraryScanner.h
    src/library/MetadataResolver.cpp
    src/library/MetadataResolver.h
    src/library/MetadataOverrideStore.cpp
    src/library/MetadataOverrideStore.h
    src/library/SidecarParser.cpp
    src/library/SidecarParser.h
    src/library/TitleScreenText.cpp
    src/library/TitleScreenText.h
    src/library/ZipDirectory.cpp
    src/library/ZipDirectory.h
)
target_include_directories(fks_library PUBLIC src)
target_link_libraries(fks_library PUBLIC fks_cdg Qt6::Core Qt6::Sql)

# Local title-screen OCR (Apple Vision on macOS). Platform code is isolated
# here; other platforms build a stub that reports no engine.
if(APPLE)
    enable_language(OBJCXX)
    add_library(fks_ocr STATIC src/ocr/PlatformTitleScreenOcr.h src/ocr/PlatformTitleScreenOcr_mac.mm)
    set_source_files_properties(src/ocr/PlatformTitleScreenOcr_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(fks_ocr PUBLIC fks_library PRIVATE "-framework Vision" "-framework CoreGraphics" "-framework Foundation")
else()
    add_library(fks_ocr STATIC src/ocr/PlatformTitleScreenOcr.h src/ocr/PlatformTitleScreenOcr_none.cpp)
    target_link_libraries(fks_ocr PUBLIC fks_library)
endif()
target_include_directories(fks_ocr PUBLIC src)

# User-owned playlists live separately from the rebuildable song catalogue.
# This target has no widgets or media-playback dependency.
add_library(fks_playlist STATIC
    src/playlist/PlaylistStore.cpp
    src/playlist/PlaylistStore.h
    src/playlist/PlaylistTypes.h
)
target_include_directories(fks_playlist PUBLIC src)
target_link_libraries(fks_playlist PUBLIC Qt6::Core Qt6::Sql PRIVATE fks_library)

add_executable(fks-catalogue tools/fks-catalogue.cpp)
# fks_ocr brings fks_library (and fks_cdg) with it.
target_link_libraries(fks-catalogue PRIVATE fks_ocr)

# Application core shared by the executable and the tests.
add_library(fks_core STATIC
    src/LibraryController.cpp
    src/LibraryController.h
    src/LibraryResultsModel.cpp
    src/LibraryResultsModel.h
    src/LibraryView.cpp
    src/LibraryView.h
    src/MetadataReviewDialog.cpp
    src/MetadataReviewDialog.h
    src/PlaylistView.cpp
    src/PlaylistView.h
    src/Logging.cpp
    src/Logging.h
    src/SongPair.cpp
    src/SongPair.h
    src/SongSettings.cpp
    src/SongSettings.h
    src/KaraokePlayer.cpp
    src/KaraokePlayer.h
    src/LyricsView.cpp
    src/LyricsView.h
    src/playlist/PlaylistPlayback.cpp
    src/playlist/PlaylistPlayback.h
    src/platform/DisplaySleepBlocker.cpp
    src/platform/DisplaySleepBlocker.h
    src/MainWindow.cpp
    src/MainWindow.h
)
target_include_directories(fks_core PUBLIC src)
target_link_libraries(fks_core PUBLIC fks_ocr fks_playlist Qt6::Core Qt6::Gui Qt6::Widgets GStreamer::GStreamer)

if(APPLE)
    target_link_libraries(fks_core PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()

add_executable(FrankiesKaraokeStudio MACOSX_BUNDLE WIN32 src/main.cpp)
target_link_libraries(FrankiesKaraokeStudio PRIVATE fks_core)
target_compile_definitions(FrankiesKaraokeStudio PRIVATE FKS_VERSION="${PROJECT_VERSION}")
set_target_properties(FrankiesKaraokeStudio PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Frankie's Karaoke Studio"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.granda.FrankiesKaraokeStudio"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

foreach(_target fks_cdg fks_library fks_ocr fks_playlist fks-catalogue fks_core FrankiesKaraokeStudio)
    if(MSVC)
        target_compile_options(${_target} PRIVATE /W4)
    else()
        target_compile_options(${_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

option(FKS_BUILD_TESTS "Build automated tests" ON)
if(FKS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
