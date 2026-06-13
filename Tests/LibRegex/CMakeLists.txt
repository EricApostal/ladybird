set(TEST_SOURCES
    TestRegex.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibRegex LIBS LibRegex)

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        get_filename_component(test_name "${source}" NAME_WE)
        set(rescan_libs)
        foreach(lib IN ITEMS LibRegex LibUnicode LibTextCodec LibURL libregex_rust libunicode_rust liburl_rust)
            if (TARGET ${lib})
                list(APPEND rescan_libs ${lib})
            endif()
        endforeach()

        if (rescan_libs)
            target_link_libraries(${test_name} PRIVATE "$<LINK_GROUP:RESCAN,${rescan_libs}>")
        endif()
    endif()
endforeach()
