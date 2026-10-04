cmake_minimum_required(VERSION 3.25)

project(pubg_vision
    VERSION 0.1.0
    DESCRIPTION "C++ computer vision learning project"
    LANGUAGES CXX
)

include(CTest)
find_package(Threads REQUIRED)

add_library(pubg_vision_core STATIC
    src/app/inference_model.cpp
    src/config/app_config.cpp
    src/core/performance_clock.cpp
    src/collection/capture_scheduler.cpp
    src/dataset/dataset_writer.cpp
    src/dataset/png_encoder.cpp
    src/dataset/trace_writer.cpp
    src/inference/model.cpp
    src/inference/mock_model.cpp
    src/inference/pipeline.cpp
    src/preprocess/preprocessor.cpp
    src/postprocess/postprocessor.cpp
    src/render/raster.cpp
)
target_include_directories(pubg_vision_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(pubg_vision_core PUBLIC cxx_std_20)
target_link_libraries(pubg_vision_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(pubg_vision_core PUBLIC /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
else()
    target_compile_options(pubg_vision_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(pubg_vision_app src/app/main.cpp src/app/inference_demo.cpp)
target_link_libraries(pubg_vision_app PRIVATE pubg_vision_core)
target_compile_definitions(pubg_vision_app PRIVATE
    PUBGVISION_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    add_library(pubg_vision_windows STATIC
        src/app/collect.cpp
        src/app/live.cpp
        src/render/windows_overlay.cpp
        src/input/windows_input.cpp
        src/capture/desktop_capture.cpp
        src/platform/windows/window_selection.cpp
    )
    target_compile_definitions(pubg_vision_windows PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX
        PRIVATE PUBGVISION_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pubg_vision_windows PUBLIC pubg_vision_core d3d11 dxgi windowscodecs ole32 user32 gdi32)
    target_link_libraries(pubg_vision_app PRIVATE pubg_vision_windows)
else()
    target_sources(pubg_vision_app PRIVATE src/capture/desktop_capture_stub.cpp src/app/collect_stub.cpp src/app/live_stub.cpp)
endif()

if(BUILD_TESTING)
    add_executable(pubg_vision_unit_tests tests/unit/core_tests.cpp)
    target_link_libraries(pubg_vision_unit_tests PRIVATE pubg_vision_core)
    add_test(NAME pubg_vision_unit_tests COMMAND pubg_vision_unit_tests)
    add_executable(pubg_vision_collection_tests tests/unit/collection_tests.cpp)
    target_link_libraries(pubg_vision_collection_tests PRIVATE pubg_vision_core)
    add_test(NAME pubg_vision_collection_tests COMMAND pubg_vision_collection_tests)
    add_executable(pubg_vision_inference_tests tests/unit/inference_tests.cpp)
    target_link_libraries(pubg_vision_inference_tests PRIVATE pubg_vision_core)
    add_test(NAME pubg_vision_inference_tests COMMAND pubg_vision_inference_tests)
    set_tests_properties(pubg_vision_inference_tests PROPERTIES TIMEOUT 30)
    set_tests_properties(pubg_vision_collection_tests PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(WIN32)
        add_executable(pubg_vision_collection_session_fixture tests/integration/collection_session_fixture.cpp)
        target_link_libraries(pubg_vision_collection_session_fixture PRIVATE pubg_vision_windows)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME pubg_vision_experiment_analysis
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/experiment_analysis_test.py")
        add_test(NAME pubg_vision_session_validation
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/session_validation_test.py")
        if(WIN32)
            add_test(NAME pubg_vision_collection_session_validation
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/collection_session_test.py"
                    "$<TARGET_FILE:pubg_vision_collection_session_fixture>")
            set_tests_properties(pubg_vision_collection_session_validation PROPERTIES TIMEOUT 30)
            add_test(NAME pubg_vision_inference_demo_validation
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/inference_demo_test.py"
                    "$<TARGET_FILE:pubg_vision_app>")
            set_tests_properties(pubg_vision_inference_demo_validation PROPERTIES TIMEOUT 30)
        endif()
    endif()

    if(WIN32)
        add_executable(pubg_vision_capture_smoke
            tests/manual/capture_smoke.cpp
        )
        target_link_libraries(pubg_vision_capture_smoke PRIVATE pubg_vision_windows gdi32 dwmapi)
        add_executable(pubg_vision_collect_smoke tests/manual/collect_smoke.cpp)
        target_link_libraries(pubg_vision_collect_smoke PRIVATE pubg_vision_windows gdi32)
        add_executable(pubg_vision_png_benchmark tests/manual/png_benchmark.cpp)
        target_link_libraries(pubg_vision_png_benchmark PRIVATE pubg_vision_windows)
        add_executable(pubg_vision_presentmon_target tests/manual/presentmon_target.cpp)
        target_link_libraries(pubg_vision_presentmon_target PRIVATE pubg_vision_windows)
    endif()
endif()
