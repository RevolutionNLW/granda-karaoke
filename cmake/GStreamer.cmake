# Locates the GStreamer SDK and defines the imported target GStreamer::GStreamer.
#
# All platform-specific GStreamer discovery lives in this file. Override the
# search by passing -DGSTREAMER_ROOT=<dir>, where <dir> contains include/ and
# lib/ (for example .../GStreamer.framework/Versions/1.0 on macOS, or
# C:/Program Files/gstreamer/1.0/msvc_x86_64 on Windows).
#
# The official SDK is required on macOS and Windows: a package-manager GStreamer may be
# missing plugins the application relies on later (for example "pitch").

set(GSTREAMER_ROOT "" CACHE PATH "GStreamer SDK root (contains include/ and lib/)")

if(NOT GSTREAMER_ROOT)
    set(_gst_candidates "")
    if(APPLE)
        list(APPEND _gst_candidates
            "$ENV{HOME}/Library/Frameworks/GStreamer.framework/Versions/1.0"
            "/Library/Frameworks/GStreamer.framework/Versions/1.0")
    elseif(WIN32)
        file(TO_CMAKE_PATH "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}" _gst_windows_root)
        list(APPEND _gst_candidates
            "${_gst_windows_root}"
            "C:/Program Files/gstreamer/1.0/msvc_x86_64")
    endif()
    foreach(_candidate IN LISTS _gst_candidates)
        if(_candidate AND EXISTS "${_candidate}/include/gstreamer-1.0/gst/gst.h")
            set(GSTREAMER_ROOT "${_candidate}" CACHE PATH
                "GStreamer SDK root (contains include/ and lib/)" FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT GSTREAMER_ROOT AND (APPLE OR WIN32))
    message(FATAL_ERROR "The official GStreamer SDK is required. Install the GStreamer framework/MSVC SDK or set GSTREAMER_ROOT to its root. Homebrew/pkg-config is not supported on this platform.")
endif()

if(GSTREAMER_ROOT)
    set(_gst_soundtouch_found FALSE)
    foreach(_plugin libgstsoundtouch.dylib libgstsoundtouch.so gstsoundtouch.dll)
        if(EXISTS "${GSTREAMER_ROOT}/lib/gstreamer-1.0/${_plugin}")
            set(_gst_soundtouch_found TRUE)
        endif()
    endforeach()
    if(NOT _gst_soundtouch_found)
        message(FATAL_ERROR "GStreamer SDK at '${GSTREAMER_ROOT}' is missing the SoundTouch plugin in lib/gstreamer-1.0. Install the complete official GStreamer SDK; Homebrew is not supported.")
    endif()
    set(_gst_audiofx_found FALSE)
    foreach(_plugin libgstaudiofx.dylib libgstaudiofx.so gstaudiofx.dll)
        if(EXISTS "${GSTREAMER_ROOT}/lib/gstreamer-1.0/${_plugin}")
            set(_gst_audiofx_found TRUE)
        endif()
    endforeach()
    if(NOT _gst_audiofx_found)
        message(FATAL_ERROR "GStreamer SDK at '${GSTREAMER_ROOT}' is missing the audiofx plugin that provides scaletempo in lib/gstreamer-1.0. Install the complete official GStreamer SDK.")
    endif()
    # Re-resolve when the SDK root changes; do not retain another installation's paths.
    foreach(_var GST_INCLUDE_DIR GLIB_INCLUDE_DIR GLIBCONFIG_INCLUDE_DIR
                 GST_LIBRARY GOBJECT_LIBRARY GLIB_LIBRARY)
        unset(${_var} CACHE)
    endforeach()
    find_path(GST_INCLUDE_DIR gst/gst.h
        PATHS "${GSTREAMER_ROOT}/include/gstreamer-1.0" NO_DEFAULT_PATH)
    find_path(GLIB_INCLUDE_DIR glib.h
        PATHS "${GSTREAMER_ROOT}/include/glib-2.0" NO_DEFAULT_PATH)
    find_path(GLIBCONFIG_INCLUDE_DIR glibconfig.h
        PATHS "${GSTREAMER_ROOT}/lib/glib-2.0/include" NO_DEFAULT_PATH)
    find_library(GST_LIBRARY gstreamer-1.0 PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(GOBJECT_LIBRARY gobject-2.0 PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(GLIB_LIBRARY glib-2.0 PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH)

    foreach(_var GST_INCLUDE_DIR GLIB_INCLUDE_DIR GLIBCONFIG_INCLUDE_DIR
                 GST_LIBRARY GOBJECT_LIBRARY GLIB_LIBRARY)
        if(NOT ${_var})
            message(FATAL_ERROR "GStreamer SDK at '${GSTREAMER_ROOT}' is incomplete: ${_var} not found")
        endif()
    endforeach()

    add_library(GStreamer::GStreamer INTERFACE IMPORTED)
    target_include_directories(GStreamer::GStreamer INTERFACE
        "${GST_INCLUDE_DIR}" "${GLIB_INCLUDE_DIR}" "${GLIBCONFIG_INCLUDE_DIR}")
    target_link_libraries(GStreamer::GStreamer INTERFACE
        "${GST_LIBRARY}" "${GOBJECT_LIBRARY}" "${GLIB_LIBRARY}")
    set(GSTREAMER_LIBRARY_DIR "${GSTREAMER_ROOT}/lib")
    message(STATUS "GStreamer SDK: ${GSTREAMER_ROOT}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux distribution packages are supported.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
    add_library(GStreamer::GStreamer ALIAS PkgConfig::GSTREAMER)
    set(GSTREAMER_LIBRARY_DIR "")
    message(STATUS "GStreamer via pkg-config: ${GSTREAMER_VERSION}")
else()
    message(FATAL_ERROR "Set GSTREAMER_ROOT to a complete GStreamer SDK on this platform.")
endif()
