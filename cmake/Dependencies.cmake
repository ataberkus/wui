function(wui_require_text_dependencies)
    if(NOT CMAKE_BUILD_TYPE)
        message(FATAL_ERROR "WUI text dependencies require a single-config CMAKE_BUILD_TYPE (Debug or Release).")
    endif()

    set(WUI_DEPENDENCY_PREFIX "${PROJECT_SOURCE_DIR}/out/deps/${CMAKE_BUILD_TYPE}")
    foreach(dependency_result
            WUI_FREETYPE_INCLUDE_DIR WUI_FREETYPE_LIBRARY
            WUI_HARFBUZZ_INCLUDE_DIR WUI_HARFBUZZ_LIBRARY
            WUI_UTF8PROC_INCLUDE_DIR WUI_UTF8PROC_LIBRARY)
        unset(${dependency_result} CACHE)
        unset(${dependency_result})
    endforeach()
    find_path(WUI_FREETYPE_INCLUDE_DIR ft2build.h
        PATH_SUFFIXES freetype2
        PATHS "${WUI_DEPENDENCY_PREFIX}/include" NO_DEFAULT_PATH NO_CACHE)
    find_library(WUI_FREETYPE_LIBRARY NAMES freetype freetyped
        PATH_SUFFIXES "${CMAKE_LIBRARY_ARCHITECTURE}"
        PATHS "${WUI_DEPENDENCY_PREFIX}/lib" NO_DEFAULT_PATH NO_CACHE)
    find_path(WUI_HARFBUZZ_INCLUDE_DIR hb.h
        PATH_SUFFIXES harfbuzz
        PATHS "${WUI_DEPENDENCY_PREFIX}/include" NO_DEFAULT_PATH NO_CACHE)
    find_library(WUI_HARFBUZZ_LIBRARY NAMES harfbuzz libharfbuzz.a
        PATH_SUFFIXES "${CMAKE_LIBRARY_ARCHITECTURE}"
        PATHS "${WUI_DEPENDENCY_PREFIX}/lib" NO_DEFAULT_PATH NO_CACHE)
    find_path(WUI_UTF8PROC_INCLUDE_DIR utf8proc.h
        PATHS "${WUI_DEPENDENCY_PREFIX}/include" NO_DEFAULT_PATH NO_CACHE)
    find_library(WUI_UTF8PROC_LIBRARY NAMES utf8proc utf8proc_static
        PATH_SUFFIXES "${CMAKE_LIBRARY_ARCHITECTURE}"
        PATHS "${WUI_DEPENDENCY_PREFIX}/lib" NO_DEFAULT_PATH NO_CACHE)

    if(NOT WUI_FREETYPE_INCLUDE_DIR OR NOT WUI_FREETYPE_LIBRARY OR
       NOT WUI_HARFBUZZ_INCLUDE_DIR OR NOT WUI_HARFBUZZ_LIBRARY OR
       NOT WUI_UTF8PROC_INCLUDE_DIR OR NOT WUI_UTF8PROC_LIBRARY)
        message(FATAL_ERROR
            "Prepared text dependencies are missing from ${WUI_DEPENDENCY_PREFIX}. "
            "Run tools/prepare_dependencies.py --fetch --build --config ${CMAKE_BUILD_TYPE} explicitly.")
    endif()

    if(NOT TARGET wui_text_dependencies)
        add_library(wui_text_dependencies INTERFACE)
    endif()
    set_property(TARGET wui_text_dependencies PROPERTY
        INTERFACE_COMPILE_DEFINITIONS UTF8PROC_STATIC)
    # No public wui header includes these, so installed consumers never need them.
    set_property(TARGET wui_text_dependencies PROPERTY INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${WUI_FREETYPE_INCLUDE_DIR}>"
        "$<BUILD_INTERFACE:${WUI_HARFBUZZ_INCLUDE_DIR}>"
        "$<BUILD_INTERFACE:${WUI_UTF8PROC_INCLUDE_DIR}>")
    # Installed copies are renamed to <name>.lib and imported by wuiDependencies.cmake.
    set_property(TARGET wui_text_dependencies PROPERTY INTERFACE_LINK_LIBRARIES
        "$<BUILD_INTERFACE:${WUI_FREETYPE_LIBRARY}>"
        "$<BUILD_INTERFACE:${WUI_HARFBUZZ_LIBRARY}>"
        "$<BUILD_INTERFACE:${WUI_UTF8PROC_LIBRARY}>"
        "$<INSTALL_INTERFACE:wui::freetype>"
        "$<INSTALL_INTERFACE:wui::harfbuzz>"
        "$<INSTALL_INTERFACE:wui::utf8proc>")
    foreach(name IN ITEMS freetype harfbuzz utf8proc)
        string(TOUPPER "${name}" upper)
        install(FILES "${WUI_${upper}_LIBRARY}"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/wui-deps/${CMAKE_BUILD_TYPE}" RENAME "${name}.lib")
    endforeach()
endfunction()
