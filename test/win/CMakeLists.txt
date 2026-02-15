#[[
/******************************************************************************
    Modifications Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>
    Modified: 2026-02-15
    Notes: Changes for Syndy Creator Studio.
******************************************************************************/
]]

project(win-test)

option(ENABLE_WIN_TEST_SAMPLE "Build legacy Win32 rendering sample" OFF)

set(obs_test_obs_deps_bin "")
foreach(prefix_path IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${prefix_path}/share/obs-deps/VERSION" AND EXISTS "${prefix_path}/bin")
    set(obs_test_obs_deps_bin "${prefix_path}/bin")
    break()
  endif()
endforeach()

function(register_win_test target_name)
  string(REPLACE ";" "\\;" escaped_system_path "$ENV{PATH}")
  set(test_path "$<TARGET_FILE_DIR:${target_name}>")
  if(TARGET w32-pthreads)
    string(APPEND test_path "\\;$<TARGET_FILE_DIR:w32-pthreads>")
  endif()
  if(obs_test_obs_deps_bin)
    string(APPEND test_path "\\;${obs_test_obs_deps_bin}")
  endif()
  string(APPEND test_path "\\;${escaped_system_path}")

  add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
  set_tests_properties(${target_name} PROPERTIES
    WORKING_DIRECTORY $<TARGET_FILE_DIR:${target_name}>
    ENVIRONMENT "PATH=${test_path}"
  )
endfunction()

if(ENABLE_WIN_TEST_SAMPLE)
  add_executable(win-test WIN32)

  target_sources(win-test PRIVATE test.cpp)

  target_link_libraries(win-test PRIVATE OBS::libobs)

  set_target_properties(win-test PROPERTIES FOLDER "tests and examples")

  if(COMMAND define_graphic_modules)
    define_graphic_modules(win-test)
  endif()
endif()

set(module_progress_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/module-progress-fixtures/$<CONFIG>")

add_library(module-progress-good MODULE module-progress-good.c)
target_link_libraries(module-progress-good PRIVATE OBS::libobs)
set_target_properties(module-progress-good PROPERTIES
  FOLDER "tests and examples"
  PREFIX ""
  OUTPUT_NAME "mlp-good"
  RUNTIME_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  LIBRARY_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  ARCHIVE_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
)

add_library(module-progress-fail-init MODULE module-progress-fail-init.c)
target_link_libraries(module-progress-fail-init PRIVATE OBS::libobs)
set_target_properties(module-progress-fail-init PROPERTIES
  FOLDER "tests and examples"
  PREFIX ""
  OUTPUT_NAME "mlp-fail-init"
  RUNTIME_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  LIBRARY_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  ARCHIVE_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
)

add_library(module-progress-missing-exports MODULE module-progress-missing-exports.c)
set_target_properties(module-progress-missing-exports PROPERTIES
  FOLDER "tests and examples"
  PREFIX ""
  OUTPUT_NAME "mlp-missing-exports"
  RUNTIME_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  LIBRARY_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  ARCHIVE_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
)

add_library(module-progress-not-plugin MODULE module-progress-not-plugin.c)
set_target_properties(module-progress-not-plugin PROPERTIES
  FOLDER "tests and examples"
  PREFIX ""
  OUTPUT_NAME "mlp-not-plugin"
  RUNTIME_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  LIBRARY_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
  ARCHIVE_OUTPUT_DIRECTORY "${module_progress_fixture_dir}"
)

add_executable(module-load-progress-test module-load-progress-test.cpp)
target_link_libraries(module-load-progress-test PRIVATE OBS::libobs)
target_compile_definitions(module-load-progress-test PRIVATE
  MODULE_PROGRESS_FIXTURE_DIR="$<TARGET_FILE_DIR:module-progress-good>"
)
set_target_properties(module-load-progress-test PROPERTIES
  FOLDER "tests and examples"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libobs/$<CONFIG>"
)

add_dependencies(module-load-progress-test
  module-progress-good
  module-progress-fail-init
  module-progress-missing-exports
  module-progress-not-plugin
)
register_win_test(module-load-progress-test)

add_executable(
  startup-progress-model-test
  startup-progress-model-test.cpp
  ../../frontend/utility/StartupProgressModel.cpp
)
target_include_directories(startup-progress-model-test PRIVATE ${CMAKE_SOURCE_DIR}/frontend)
set_target_properties(startup-progress-model-test PROPERTIES
  FOLDER "tests and examples"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libobs/$<CONFIG>"
)
register_win_test(startup-progress-model-test)

add_executable(
  startup-progress-mapping-test
  startup-progress-mapping-test.cpp
  ../../frontend/utility/StartupProgressModel.cpp
)
target_include_directories(startup-progress-mapping-test PRIVATE ${CMAKE_SOURCE_DIR}/frontend)
set_target_properties(startup-progress-mapping-test PROPERTIES
  FOLDER "tests and examples"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libobs/$<CONFIG>"
)
register_win_test(startup-progress-mapping-test)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(
  startup-splash-widget-test
  startup-splash-widget-test.cpp
  ../../frontend/widgets/StartupSplashWidget.cpp
)
target_include_directories(startup-splash-widget-test PRIVATE ${CMAKE_SOURCE_DIR}/frontend)
target_link_libraries(startup-splash-widget-test PRIVATE Qt::Widgets)
set_target_properties(startup-splash-widget-test PROPERTIES
  FOLDER "tests and examples"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libobs/$<CONFIG>"
)
register_win_test(startup-splash-widget-test)

add_executable(
  startup-splash-guard-test
  startup-splash-guard-test.cpp
)
target_include_directories(startup-splash-guard-test PRIVATE ${CMAKE_SOURCE_DIR}/frontend)
set_target_properties(startup-splash-guard-test PROPERTIES
  FOLDER "tests and examples"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libobs/$<CONFIG>"
)
register_win_test(startup-splash-guard-test)

set(scene_3d_draco_fixture_dir "${CMAKE_SOURCE_DIR}/test/test-input/data/scene-3d")

add_executable(
  scene-3d-draco-loader-test
  scene-3d-draco-loader-test.cpp
  ../../plugins/scene-3d-source/scene-3d-gltf-loader.c
  ../../plugins/scene-3d-source/scene-3d-gltf-loader.h
)
target_include_directories(
  scene-3d-draco-loader-test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/scene-3d-source
)
target_link_libraries(scene-3d-draco-loader-test PRIVATE OBS::libobs)
target_compile_definitions(
  scene-3d-draco-loader-test
  PRIVATE
    SCENE_3D_DRACO_FIXTURE_DIR="${scene_3d_draco_fixture_dir}"
)
set_target_properties(scene-3d-draco-loader-test PROPERTIES
  FOLDER "tests and examples"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libobs/$<CONFIG>"
)
register_win_test(scene-3d-draco-loader-test)
