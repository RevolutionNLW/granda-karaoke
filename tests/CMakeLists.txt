find_package(Qt6 REQUIRED COMPONENTS Test)

set(FKS_TEST_MEDIA_DIR "$ENV{HOME}/Music/Frankies-Karaoke-Studio" CACHE PATH
    "External karaoke test media directory (never copied into the repository)")

function(fks_add_test name)
    cmake_parse_arguments(ARG "" "" "LIBS;ENV" ${ARGN})
    add_executable(${name} ${name}.cpp CdgTestData.h TestMedia.h BusTestPlayer.h)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} Qt6::Test)
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    if(WIN32)
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_ROOT}/bin")
    endif()
    if(ARG_ENV)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ARG_ENV}")
    endif()
endfunction()

fks_add_test(tst_cdgdecoder LIBS fks_cdg)
fks_add_test(tst_libraryparsing LIBS fks_library)
fks_add_test(tst_catalogue LIBS fks_library)
fks_add_test(tst_metadatafoundation LIBS fks_library)
fks_add_test(tst_metadataidentification LIBS fks_library)
fks_add_test(tst_songpair LIBS fks_core)
fks_add_test(tst_songsettings LIBS fks_core)
fks_add_test(tst_karaokeplayer LIBS fks_core ENV "FKS_TEST_MEDIA_DIR=${FKS_TEST_MEDIA_DIR}")
fks_add_test(tst_mainwindow LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
fks_add_test(tst_libraryview LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
fks_add_test(tst_playlists LIBS fks_playlist fks_library)
fks_add_test(tst_playlistplayback LIBS fks_core)
fks_add_test(tst_playlistview LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
fks_add_test(tst_metadatareview LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
fks_add_test(tst_librarysort LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
fks_add_test(tst_settings LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
fks_add_test(tst_splash LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")

# Closing while the library scanner is busy; the helper runs the program's
# closing sequence in a process of its own.
add_executable(tst_shutdown_helper tst_shutdown_helper.cpp)
target_link_libraries(tst_shutdown_helper PRIVATE fks_core)
fks_add_test(tst_shutdown LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
target_compile_definitions(tst_shutdown PRIVATE
    FKS_SHUTDOWN_HELPER="$<TARGET_FILE:tst_shutdown_helper>")
add_dependencies(tst_shutdown tst_shutdown_helper)
