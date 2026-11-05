set(TINYKV_TESTS
  test_batch
  test_cli
  test_codec
  test_compaction
  test_concurrency
  test_hints
  test_inspect
  test_model
  test_recovery
  test_scan
  test_segment
  test_store
)

foreach(test_name IN LISTS TINYKV_TESTS)
  add_executable(${test_name} ${test_name}.cpp)
  # Tests reach into src/ to craft and damage files at the format level.
  target_include_directories(${test_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(${test_name} PRIVATE -Wall -Wextra)
  target_link_libraries(${test_name} PRIVATE tinykv)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

target_compile_definitions(test_cli PRIVATE TINYKV_CLI_PATH="$<TARGET_FILE:tinykv_cli>")
add_dependencies(test_cli tinykv_cli)
