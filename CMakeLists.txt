cmake_minimum_required(VERSION 3.25)

project(pubg_vision
    VERSION 0.1.0
    DESCRIPTION "C++ computer vision learning project"
    LANGUAGES CXX
)

include(CTest)

add_library(pubg_vision_core STATIC src/config/app_config.cpp)
target_include_directories(pubg_vision_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(pubg_vision_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(pubg_vision_core PUBLIC /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
else()
    target_compile_options(pubg_vision_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(pubg_vision_app src/app/main.cpp)
target_link_libraries(pubg_vision_app PRIVATE pubg_vision_core)
target_compile_definitions(pubg_vision_app PRIVATE
    PUBGVISION_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    target_sources(pubg_vision_app PRIVATE
        src/capture/desktop_capture.cpp
        src/platform/windows/window_selection.cpp
    )
    target_compile_definitions(pubg_vision_app PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(pubg_vision_app PRIVATE d3d11 dxgi windowscodecs ole32 user32)
else()
    target_sources(pubg_vision_app PRIVATE src/capture/desktop_capture_stub.cpp)
endif()

if(BUILD_TESTING)
    add_executable(pubg_vision_unit_tests tests/unit/core_tests.cpp)
    target_link_libraries(pubg_vision_unit_tests PRIVATE pubg_vision_core)
    add_test(NAME pubg_vision_unit_tests COMMAND pubg_vision_unit_tests)

    if(WIN32)
        add_executable(pubg_vision_capture_smoke
            tests/manual/capture_smoke.cpp
            src/capture/desktop_capture.cpp
            src/platform/windows/window_selection.cpp
        )
        target_link_libraries(pubg_vision_capture_smoke PRIVATE pubg_vision_core
            d3d11 dxgi windowscodecs ole32 user32 gdi32 dwmapi)
        target_compile_definitions(pubg_vision_capture_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endif()
