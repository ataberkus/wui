cmake_minimum_required(VERSION 3.24)
project(wui_consumer LANGUAGES CXX)

# Deliberately lower than wui's: wui::core must propagate cxx_std_20.
set(CMAKE_CXX_STANDARD 17)

separate_arguments(components NATIVE_COMMAND "${WUI_TEST_COMPONENTS}")
find_package(wui 0.1 REQUIRED COMPONENTS ${components})

add_executable(consumer main.cpp)
foreach(component IN LISTS components)
    target_link_libraries(consumer PRIVATE wui::${component})
    string(TOUPPER "${component}" upper)
    target_compile_definitions(consumer PRIVATE WUI_HAS_${upper}=1)
endforeach()
