find_package(Qt6 REQUIRED COMPONENTS Test)

set(FKS_TEST_MEDIA_DIR "$ENV{HOME}/Frankies-Karaoke-Studio" CACHE PATH
    "External karaoke test media directory (never copied into the repository)")

function(fks_add_test name)
    cmake_parse_arguments(ARG "" "" "LIBS;ENV" ${ARGN})
    add_executable(${name} ${name}.cpp CdgTestData.h TestMedia.h BusTestPlayer.h)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} Qt6::Test)
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
fks_add_test(tst_songpair LIBS fks_core)
fks_add_test(tst_karaokeplayer LIBS fks_core ENV "FKS_TEST_MEDIA_DIR=${FKS_TEST_MEDIA_DIR}")
fks_add_test(tst_mainwindow LIBS fks_core ENV "QT_QPA_PLATFORM=offscreen")
