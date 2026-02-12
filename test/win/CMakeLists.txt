project(win-test)

option(ENABLE_WIN_TEST_SAMPLE "Build legacy Win32 rendering sample" OFF)

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
