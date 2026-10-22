cmake_minimum_required(VERSION 3.24)
project(wui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WUI_BUILD_TEXT "Build the text module" OFF)
option(WUI_BUILD_WIN32 "Build the Win32 platform adapter" OFF)
option(WUI_BUILD_D3D11 "Build the D3D11 renderer" OFF)
option(WUI_BUILD_D3D9 "Build the D3D9 renderer" OFF)
option(WUI_BUILD_D3D10 "Build the D3D10 renderer" OFF)
option(WUI_BUILD_D3D12 "Build the D3D12 renderer" OFF)
option(WUI_BUILD_VULKAN "Build the Vulkan renderer" OFF)
option(WUI_BUILD_TESTS "Build wui tests" ON)
option(WUI_BUILD_EXAMPLES "Build the widget gallery example" OFF)

if(NOT WIN32 AND (WUI_BUILD_WIN32 OR WUI_BUILD_D3D11 OR WUI_BUILD_D3D9 OR WUI_BUILD_D3D10 OR WUI_BUILD_D3D12))
    message(FATAL_ERROR "WUI_BUILD_WIN32 and the Direct3D renderers require Windows.")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(WUI_BUILD_TEXT)
    include(cmake/Dependencies.cmake)
    wui_require_text_dependencies()
endif()

add_library(wui_core STATIC
    src/core/context.cpp
    src/core/frame.cpp
    src/core/id.cpp
    src/core/resources.cpp
    src/core/types.cpp
    src/input/interaction.cpp
    src/layout/flow.cpp
    src/paint/packet.cpp
    src/paint/shapes.cpp
    src/style/animation.cpp
    src/style/theme.cpp
    src/widgets/panel.cpp
    src/widgets/row.cpp
    src/widgets/scroll.cpp
    src/widgets/text.cpp
    src/widgets/custom.cpp
    src/widgets/icon_button.cpp
    src/widgets/choice.cpp
    src/widgets/popup.cpp
    src/widgets/tooltip.cpp
    src/widgets/slider.cpp
    src/widgets/text_input.cpp
    src/widgets/numeric_input.cpp
    src/widgets/dropdown.cpp
    src/widgets/button.cpp
    src/widgets/checkbox.cpp
    src/widgets/area.cpp
    src/widgets/color_picker.cpp)
configure_file(include/wui/version.hpp.in "${PROJECT_BINARY_DIR}/include/wui/version.hpp" @ONLY)
target_include_directories(wui_core PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(wui_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(wui_core PUBLIC /utf-8)
endif()

if(WUI_BUILD_TEXT)
    add_library(wui_text STATIC src/text/font.cpp src/text/shaping.cpp src/text/atlas.cpp)
    target_link_libraries(wui_text PUBLIC wui_core PRIVATE wui_text_dependencies)
endif()

if(WUI_BUILD_WIN32)
    add_library(wui_win32 STATIC
        backends/platform/win32.cpp backends/platform/win32_services.cpp)
    target_link_libraries(wui_win32 PUBLIC wui_core PRIVATE user32 gdi32 imm32)
endif()
if(WUI_BUILD_D3D11)
    file(READ "${PROJECT_SOURCE_DIR}/backends/renderer/d3d11_shaders.hlsl" D3D11_SHADER_SOURCE)
    configure_file("${PROJECT_SOURCE_DIR}/backends/renderer/d3d11_shader_source.hpp.in"
        "${PROJECT_BINARY_DIR}/generated/wui_d3d11_shader_source.hpp" @ONLY)
    add_library(wui_d3d11 STATIC backends/renderer/d3d11.cpp backends/renderer/d3d11_state.cpp)
    target_include_directories(wui_d3d11 PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(wui_d3d11 PUBLIC wui_core PRIVATE d3d11 d3dcompiler)
endif()
if(WUI_BUILD_D3D9)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/backends/renderer/d3d9_shaders.hlsl")
    file(READ "${PROJECT_SOURCE_DIR}/backends/renderer/d3d9_shaders.hlsl" D3D9_SHADER_SOURCE)
    configure_file("${PROJECT_SOURCE_DIR}/backends/renderer/d3d9_shader_source.hpp.in"
        "${PROJECT_BINARY_DIR}/generated/wui_d3d9_shader_source.hpp" @ONLY)
    add_library(wui_d3d9 STATIC
        backends/renderer/d3d9.cpp backends/renderer/d3d9_chunks.cpp
        backends/renderer/d3d9_state.cpp)
    target_include_directories(wui_d3d9 PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(wui_d3d9 PUBLIC wui_core PRIVATE d3d9 d3dcompiler)
endif()
if(WUI_BUILD_D3D10)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/backends/renderer/d3d10_shaders.hlsl")
    file(READ "${PROJECT_SOURCE_DIR}/backends/renderer/d3d10_shaders.hlsl" D3D10_SHADER_SOURCE)
    configure_file("${PROJECT_SOURCE_DIR}/backends/renderer/d3d10_shader_source.hpp.in"
        "${PROJECT_BINARY_DIR}/generated/wui_d3d10_shader_source.hpp" @ONLY)
    add_library(wui_d3d10 STATIC backends/renderer/d3d10.cpp backends/renderer/d3d10_state.cpp)
    target_include_directories(wui_d3d10 PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(wui_d3d10 PUBLIC wui_core PRIVATE d3d10_1 d3dcompiler)
endif()
if(WUI_BUILD_D3D12)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/backends/renderer/d3d12_shaders.hlsl")
    file(READ "${PROJECT_SOURCE_DIR}/backends/renderer/d3d12_shaders.hlsl" D3D12_SHADER_SOURCE)
    configure_file("${PROJECT_SOURCE_DIR}/backends/renderer/d3d12_shader_source.hpp.in"
        "${PROJECT_BINARY_DIR}/generated/wui_d3d12_shader_source.hpp" @ONLY)
    add_library(wui_d3d12 STATIC backends/renderer/d3d12.cpp
        backends/renderer/d3d12_descriptors.cpp backends/renderer/d3d12_upload.cpp)
    target_include_directories(wui_d3d12 PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(wui_d3d12 PUBLIC wui_core PRIVATE d3d12 dxgi d3dcompiler)
endif()
if(WUI_BUILD_VULKAN)
    find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(WUI_SPIRV_VAL NAMES spirv-val HINTS "$ENV{VULKAN_SDK}/Bin")
    set(WUI_VULKAN_SHADER_HEADERS)
    foreach(stage IN ITEMS vert frag)
        set(shader_source "${PROJECT_SOURCE_DIR}/backends/renderer/vulkan.${stage}")
        set(shader_spv "${PROJECT_BINARY_DIR}/generated/wui_vulkan_${stage}.spv")
        set(shader_header "${PROJECT_BINARY_DIR}/generated/wui_vulkan_${stage}.hpp")
        if(stage STREQUAL "vert")
            set(shader_symbol kVertexSpirv)
        else()
            set(shader_symbol kFragmentSpirv)
        endif()
        set(validate_spirv)
        if(WUI_SPIRV_VAL)
            list(APPEND validate_spirv
                COMMAND "${WUI_SPIRV_VAL}" --target-env vulkan1.1 "${shader_spv}")
        endif()
        add_custom_command(OUTPUT "${shader_header}" BYPRODUCTS "${shader_spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/generated"
            COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.1
                -o "${shader_spv}" "${shader_source}"
            ${validate_spirv}
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/embed_spirv.py"
                "${shader_spv}" "${shader_header}" "${shader_symbol}"
            DEPENDS "${shader_source}" "${PROJECT_SOURCE_DIR}/tools/embed_spirv.py"
            VERBATIM)
        list(APPEND WUI_VULKAN_SHADER_HEADERS "${shader_header}")
    endforeach()
    add_library(wui_vulkan STATIC
        backends/renderer/vulkan.cpp backends/renderer/vulkan_memory.cpp
        backends/renderer/vulkan_pipeline.cpp ${WUI_VULKAN_SHADER_HEADERS})
    target_include_directories(wui_vulkan PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(wui_vulkan PUBLIC wui_core Vulkan::Vulkan)
endif()

# Package: wui::<component> in-tree aliases and an installed, relocatable export.
set(WUI_PACKAGE_COMPONENTS)
set(WUI_INSTALLED_TARGETS)
foreach(component IN ITEMS core text win32 d3d9 d3d10 d3d11 d3d12 vulkan)
    if(TARGET wui_${component})
        add_library(wui::${component} ALIAS wui_${component})
        set_target_properties(wui_${component} PROPERTIES EXPORT_NAME ${component} DEBUG_POSTFIX d)
        list(APPEND WUI_PACKAGE_COMPONENTS ${component})
        list(APPEND WUI_INSTALLED_TARGETS wui_${component})
    endif()
endforeach()
if(TARGET wui_text_dependencies)
    set_target_properties(wui_text_dependencies PROPERTIES EXPORT_NAME text_dependencies)
    list(APPEND WUI_INSTALLED_TARGETS wui_text_dependencies)
endif()
install(TARGETS ${WUI_INSTALLED_TARGETS} EXPORT wuiTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/wui DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp" PATTERN "backends" EXCLUDE)
install(FILES "${PROJECT_BINARY_DIR}/include/wui/version.hpp" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wui)
foreach(component IN ITEMS win32 d3d9 d3d10 d3d11 d3d12 vulkan)
    if(TARGET wui_${component})
        install(FILES include/wui/backends/${component}.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wui/backends)
    endif()
endforeach()
install(EXPORT wuiTargets NAMESPACE wui:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wui)
configure_package_config_file(cmake/wuiConfig.cmake.in "${PROJECT_BINARY_DIR}/wuiConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wui)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/wuiConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
file(WRITE "${PROJECT_BINARY_DIR}/wuiComponents-${CMAKE_BUILD_TYPE}.cmake"
    "set(wui_COMPONENTS_${CMAKE_BUILD_TYPE} ${WUI_PACKAGE_COMPONENTS})\n")
install(FILES "${PROJECT_BINARY_DIR}/wuiConfig.cmake" "${PROJECT_BINARY_DIR}/wuiConfigVersion.cmake"
    "${PROJECT_BINARY_DIR}/wuiComponents-${CMAKE_BUILD_TYPE}.cmake" cmake/wuiDependencies.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wui)
install(FILES LICENSE THIRD_PARTY_NOTICES.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(WUI_BUILD_TEXT)
    install(FILES assets/fonts/Inter-Regular.ttf assets/fonts/Inter-SemiBold.ttf assets/fonts/LICENSE.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wui/fonts)
endif()

if(WUI_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    set(WUI_DOCTEST_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/third_party/doctest")
    if(NOT EXISTS "${WUI_DOCTEST_INCLUDE_DIR}/doctest/doctest.h")
        message(FATAL_ERROR
            "Pinned doctest source is missing. Run tools/prepare_dependencies.py --fetch --only doctest explicitly.")
    endif()
    add_executable(wui_tests
        tests/test_main.cpp
        tests/core/types_test.cpp
        tests/core/id_test.cpp
        tests/input/interaction_test.cpp
        tests/layout/flow_test.cpp
        tests/paint/packet_test.cpp
        tests/paint/shapes_test.cpp
        tests/style/theme_test.cpp
        tests/paint/panel_recording_test.cpp
        tests/widgets/foundation_test.cpp
        tests/widgets/choice_test.cpp
        tests/widgets/popup_test.cpp
        tests/widgets/area_test.cpp
        tests/paint/polygon_test.cpp
        tests/conformance/compare_test.cpp
        tests/renderers/scissor_test.cpp
        tests/core/version_test.cpp
        tests/conformance/compare.cpp
        examples/widget_gallery/gallery.cpp)
    target_include_directories(wui_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}"
        "${PROJECT_SOURCE_DIR}/examples/widget_gallery")
    target_link_libraries(wui_tests PRIVATE wui_core Threads::Threads)
    target_compile_definitions(wui_tests PRIVATE WUI_PROJECT_VERSION="${PROJECT_VERSION}")
    add_test(NAME wui-core COMMAND wui_tests)
    if(WUI_BUILD_TEXT)
        add_executable(wui_text_tests tests/test_main.cpp tests/text/text_test.cpp tests/text/editing_test.cpp)
        target_include_directories(wui_text_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_text_tests PRIVATE wui_text wui_text_dependencies)
        target_compile_definitions(wui_text_tests PRIVATE
            WUI_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
        add_test(NAME wui-text COMMAND wui_text_tests)
        add_executable(wui_replay_tests tests/test_main.cpp tests/widgets/scenario_replay_test.cpp
            examples/widget_gallery/gallery.cpp examples/widget_gallery/scenario_replay.cpp)
        target_include_directories(wui_replay_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}"
            "${PROJECT_SOURCE_DIR}/examples/widget_gallery")
        target_link_libraries(wui_replay_tests PRIVATE wui_text wui_text_dependencies)
        target_compile_definitions(wui_replay_tests PRIVATE
            WUI_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
        add_test(NAME wui-replay COMMAND wui_replay_tests)
    endif()
    if(WUI_BUILD_WIN32)
        add_executable(wui_win32_tests tests/test_main.cpp tests/platform/win32_test.cpp)
        target_include_directories(wui_win32_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_win32_tests PRIVATE wui_win32 imm32)
        add_test(NAME wui-win32 COMMAND wui_win32_tests)
        # Clipboard tests own a private window station, never the user's clipboard.
        add_executable(wui_win32_clipboard_tests tests/platform/win32_clipboard_test.cpp)
        target_include_directories(wui_win32_clipboard_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_win32_clipboard_tests PRIVATE wui_win32 imm32 advapi32 Threads::Threads)
        add_test(NAME wui-win32-clipboard COMMAND wui_win32_clipboard_tests)
    endif()
    if(WUI_BUILD_D3D11)
        add_executable(wui_d3d11_tests tests/renderers/d3d11_test.cpp tests/support/packet_fixture.cpp tests/support/native_image.cpp)
        target_include_directories(wui_d3d11_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_d3d11_tests PRIVATE wui_d3d11 wui_core d3d11 dxgi d3dcompiler)
        add_test(NAME wui-d3d11 COMMAND wui_d3d11_tests)
    endif()
    if(WUI_BUILD_D3D9)
        add_executable(wui_d3d9_chunks_tests
            tests/test_main.cpp tests/renderers/d3d9_chunks_test.cpp)
        target_include_directories(wui_d3d9_chunks_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_d3d9_chunks_tests PRIVATE wui_d3d9)
        add_test(NAME wui-d3d9-chunks COMMAND wui_d3d9_chunks_tests)
        add_executable(wui_d3d9_tests tests/test_main.cpp tests/renderers/d3d9_test.cpp
            tests/support/packet_fixture.cpp tests/support/native_image.cpp)
        target_include_directories(wui_d3d9_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_d3d9_tests PRIVATE wui_d3d9 d3d9 user32)
        if(WUI_BUILD_TEXT)
            target_sources(wui_d3d9_tests PRIVATE examples/widget_gallery/gallery.cpp)
            target_include_directories(wui_d3d9_tests PRIVATE "${PROJECT_SOURCE_DIR}/examples/widget_gallery")
            target_link_libraries(wui_d3d9_tests PRIVATE wui_text)
            target_compile_definitions(wui_d3d9_tests PRIVATE WUI_PROBE_TEXT=1
                WUI_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
        endif()
        add_test(NAME wui-d3d9 COMMAND wui_d3d9_tests)
    endif()
    if(WUI_BUILD_D3D10)
        add_executable(wui_d3d10_tests tests/test_main.cpp tests/renderers/d3d10_test.cpp
            tests/support/packet_fixture.cpp tests/support/native_image.cpp)
        target_include_directories(wui_d3d10_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_d3d10_tests PRIVATE wui_d3d10 d3d10_1 dxgi)
        add_test(NAME wui-d3d10 COMMAND wui_d3d10_tests)
    endif()
    if(WUI_BUILD_D3D12)
        add_executable(wui_d3d12_lifetime_tests tests/test_main.cpp
            tests/renderers/d3d12_lifetime_test.cpp backends/renderer/d3d12_descriptors.cpp
            backends/renderer/d3d12_upload.cpp)
        target_include_directories(wui_d3d12_lifetime_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}"
            "${PROJECT_SOURCE_DIR}/backends/renderer")
        target_link_libraries(wui_d3d12_lifetime_tests PRIVATE wui_core)
        add_test(NAME wui-d3d12-lifetime COMMAND wui_d3d12_lifetime_tests)
        add_executable(wui_d3d12_tests tests/test_main.cpp tests/renderers/d3d12_test.cpp
            tests/support/packet_fixture.cpp tests/support/native_image.cpp)
        target_include_directories(wui_d3d12_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_d3d12_tests PRIVATE wui_d3d12 d3d12 dxgi)
        add_test(NAME wui-d3d12 COMMAND wui_d3d12_tests)
    endif()
    if(WUI_BUILD_VULKAN)
        add_executable(wui_vulkan_lifetime_tests
            tests/test_main.cpp tests/renderers/vulkan_lifetime_test.cpp
            backends/renderer/vulkan_memory.cpp)
        target_include_directories(wui_vulkan_lifetime_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_vulkan_lifetime_tests PRIVATE wui_core)
        add_test(NAME wui-vulkan-lifetime COMMAND wui_vulkan_lifetime_tests)
        add_executable(wui_vulkan_tests
            tests/test_main.cpp tests/renderers/vulkan_test.cpp
            tests/support/packet_fixture.cpp tests/support/native_image.cpp)
        target_include_directories(wui_vulkan_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
        target_link_libraries(wui_vulkan_tests PRIVATE wui_vulkan)
        if(WUI_BUILD_TEXT)
            target_sources(wui_vulkan_tests PRIVATE examples/widget_gallery/gallery.cpp)
            target_include_directories(wui_vulkan_tests PRIVATE "${PROJECT_SOURCE_DIR}/examples/widget_gallery")
            target_link_libraries(wui_vulkan_tests PRIVATE wui_text)
            target_compile_definitions(wui_vulkan_tests PRIVATE WUI_PROBE_TEXT=1
                WUI_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
        endif()
        add_test(NAME wui-vulkan COMMAND wui_vulkan_tests)
    endif()

    # Common backend conformance suite: one executable per enabled renderer.
    function(wui_add_conformance api)
        add_executable(wui_conformance_${api} tests/conformance/conformance.cpp
            tests/conformance/compare.cpp tests/conformance/${api}_backend.cpp
            tests/support/packet_fixture.cpp examples/widget_gallery/gallery.cpp
            examples/widget_gallery/scenario_replay.cpp)
        target_include_directories(wui_conformance_${api} PRIVATE
            "${PROJECT_SOURCE_DIR}/examples/widget_gallery")
        target_link_libraries(wui_conformance_${api} PRIVATE wui_text wui_text_dependencies
            wui_${api} ${ARGN})
        target_compile_definitions(wui_conformance_${api} PRIVATE
            WUI_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
    endfunction()
    # Each run writes <binary>/conformance/<name>; D3D11 WARP is the reference.
    set(WUI_CONFORMANCE_CANDIDATES)
    function(wui_conformance_run api name)
        add_test(NAME wui-conformance-${name} COMMAND wui_conformance_${api} ${ARGN}
            --out "${PROJECT_BINARY_DIR}/conformance/${name}")
        set_tests_properties(wui-conformance-${name} PROPERTIES FIXTURES_SETUP wui_conformance)
        if(NOT name STREQUAL "d3d11-warp")
            set(WUI_CONFORMANCE_CANDIDATES ${WUI_CONFORMANCE_CANDIDATES}
                --candidate "${PROJECT_BINARY_DIR}/conformance/${name}" PARENT_SCOPE)
        endif()
    endfunction()
    option(WUI_CONFORMANCE_HARDWARE "Also run conformance on the default hardware adapter" OFF)
    if(WUI_BUILD_TEXT AND WUI_BUILD_D3D11)
        wui_add_conformance(d3d11 d3d11 dxgi)
        wui_conformance_run(d3d11 d3d11-warp --driver warp)
        if(WUI_CONFORMANCE_HARDWARE)
            wui_conformance_run(d3d11 d3d11-hardware --driver hardware)
        endif()
    endif()
    if(WUI_BUILD_TEXT AND WUI_BUILD_D3D10)
        wui_add_conformance(d3d10 d3d10_1 dxgi)
        wui_conformance_run(d3d10 d3d10-warp --driver warp)
        if(WUI_CONFORMANCE_HARDWARE)
            wui_conformance_run(d3d10 d3d10-hardware --driver hardware)
        endif()
    endif()
    if(WUI_BUILD_TEXT AND WUI_BUILD_D3D9)
        # D3D9 has no software rasterizer: the suite always runs on HAL.
        wui_add_conformance(d3d9 d3d9 user32)
        wui_conformance_run(d3d9 d3d9-hardware)
    endif()
    if(WUI_BUILD_TEXT AND WUI_BUILD_VULKAN)
        wui_add_conformance(vulkan)
        wui_conformance_run(vulkan vulkan-hardware)
    endif()
    if(WUI_BUILD_TEXT AND WUI_BUILD_D3D12)
        wui_add_conformance(d3d12 d3d12 dxgi)
        wui_conformance_run(d3d12 d3d12-warp --driver warp)
        if(WUI_CONFORMANCE_HARDWARE)
            wui_conformance_run(d3d12 d3d12-hardware --driver hardware)
        endif()
    endif()
    if(WUI_BUILD_TEXT AND WUI_BUILD_D3D11 AND WUI_CONFORMANCE_CANDIDATES)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME wui-conformance-compare
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/compare_conformance.py"
                --reference "${PROJECT_BINARY_DIR}/conformance/d3d11-warp"
                ${WUI_CONFORMANCE_CANDIDATES}
                --report "${PROJECT_BINARY_DIR}/conformance/comparison.md")
        set_tests_properties(wui-conformance-compare PROPERTIES FIXTURES_REQUIRED wui_conformance)
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # Compile every assertion-bearing implementation unit in both modes.
    # The remaining core primitives have no contract asserts and are shared.
    foreach(mode IN ITEMS release debug)
        add_library(wui_contract_${mode}_core STATIC
            src/core/context.cpp src/core/frame.cpp src/widgets/panel.cpp
            src/widgets/row.cpp src/widgets/scroll.cpp src/widgets/custom.cpp
            src/widgets/choice.cpp)
        target_link_libraries(wui_contract_${mode}_core PUBLIC wui_core)
    endforeach()
    target_compile_definitions(wui_contract_release_core PRIVATE NDEBUG)
    if(MSVC)
        target_compile_options(wui_contract_debug_core PRIVATE /UNDEBUG)
    else()
        target_compile_options(wui_contract_debug_core PRIVATE -UNDEBUG)
    endif()
    add_executable(wui_contract_tests tests/test_main.cpp tests/widgets/contract_test.cpp)
    target_include_directories(wui_contract_tests PRIVATE "${WUI_DOCTEST_INCLUDE_DIR}")
    target_compile_definitions(wui_contract_tests PRIVATE NDEBUG)
    target_link_libraries(wui_contract_tests PRIVATE wui_contract_release_core)
    add_test(NAME wui-contract-release COMMAND wui_contract_tests)
    add_executable(wui_allocation_probe tests/widgets/allocation_probe.cpp)
    target_link_libraries(wui_allocation_probe PRIVATE wui_core)
    add_test(NAME wui-allocation-failure COMMAND wui_allocation_probe)
    add_test(NAME wui-semantic-reuse COMMAND wui_allocation_probe --semantic-reuse)
    add_test(NAME wui-frame-reuse COMMAND wui_allocation_probe --frame-reuse)
    add_test(NAME wui-allocation-contracts COMMAND wui_allocation_probe --allocation-contracts)
    add_executable(wui_contract_probe tests/widgets/contract_probe.cpp)
    target_link_libraries(wui_contract_probe PRIVATE wui_contract_debug_core)
    if(MSVC)
        target_compile_options(wui_contract_probe PRIVATE /UNDEBUG)
    else()
        target_compile_options(wui_contract_probe PRIVATE -UNDEBUG)
    endif()
    add_test(NAME wui-contract-debug
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/widgets/contract_runner.py"
            --probe $<TARGET_FILE:wui_contract_probe>)
    add_test(NAME wui-notices
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cmake/notices_test.py"
            --source "${PROJECT_SOURCE_DIR}")
    add_test(NAME wui-dependency-checkout
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cmake/dependency_checkout_test.py")
    add_test(NAME wui-dependency-prefix-reconfigure
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cmake/dependency_prefix_reconfigure_test.py"
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --make-program "${CMAKE_MAKE_PROGRAM}" --source "${PROJECT_SOURCE_DIR}")
    add_test(NAME wui-package-consumer
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cmake/package_consumer_test.py"
            --build-dir "${PROJECT_BINARY_DIR}" --source "${PROJECT_SOURCE_DIR}"
            --config "${CMAKE_BUILD_TYPE}" --components "${WUI_PACKAGE_COMPONENTS}"
            --work "${PROJECT_BINARY_DIR}/package-test" --version "${PROJECT_VERSION}"
            --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
            --make-program "${CMAKE_MAKE_PROGRAM}")
endif()
if(WUI_BUILD_EXAMPLES)
    if(NOT WIN32 OR NOT WUI_BUILD_TEXT OR NOT WUI_BUILD_WIN32 OR NOT WUI_BUILD_D3D11)
        message(FATAL_ERROR "WUI_BUILD_EXAMPLES requires Windows, text, Win32 and D3D11")
    endif()
    add_executable(wui_widget_gallery
        examples/widget_gallery/gallery.cpp
        examples/widget_gallery/native_app.cpp
        examples/widget_gallery/validation.cpp
        examples/widget_gallery/allocation_metrics.cpp
        examples/widget_gallery/main_win32_d3d11.cpp)
    target_include_directories(wui_widget_gallery PRIVATE
        "${PROJECT_SOURCE_DIR}/examples/widget_gallery"
        "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(wui_widget_gallery PRIVATE
        wui_text wui_win32 wui_d3d11 d3d11 dxgi d3dcompiler user32 gdi32 shell32 ole32 windowscodecs
        psapi)
    if(MSVC)
        target_compile_options(wui_widget_gallery PRIVATE /utf-8)
    endif()
    # Recorded verbatim in measurement reports.
    target_compile_definitions(wui_widget_gallery PRIVATE
        "WUI_GALLERY_CXX_FLAGS=\"${CMAKE_CXX_FLAGS} $<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}>\"")
    add_custom_command(TARGET wui_widget_gallery POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:wui_widget_gallery>/assets/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf"
            "${PROJECT_SOURCE_DIR}/assets/fonts/Inter-SemiBold.ttf"
            "${PROJECT_SOURCE_DIR}/assets/fonts/LICENSE.txt"
            "$<TARGET_FILE_DIR:wui_widget_gallery>/assets/fonts")
    if(WUI_BUILD_TESTS)
        add_test(NAME wui-gallery-cli
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/example/gallery_cli_test.py"
                $<TARGET_FILE:wui_widget_gallery>)
    endif()
endif()
