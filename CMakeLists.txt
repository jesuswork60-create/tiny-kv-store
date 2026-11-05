cmake_minimum_required(VERSION 3.20)
project(tiny_kv_store VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Threads REQUIRED)

add_library(tinykv STATIC
  src/compaction.cpp
  src/crc32.cpp
  src/file.cpp
  src/hint.cpp
  src/inspect.cpp
  src/keydir.cpp
  src/record.cpp
  src/recovery.cpp
  src/segment.cpp
  src/store.cpp
)
target_include_directories(tinykv
  PUBLIC include
  PRIVATE src
)
target_compile_options(tinykv PRIVATE -Wall -Wextra)
target_link_libraries(tinykv PUBLIC Threads::Threads)

add_executable(tinykv_cli cli/main.cpp)
set_target_properties(tinykv_cli PROPERTIES OUTPUT_NAME tinykv)
target_compile_options(tinykv_cli PRIVATE -Wall -Wextra)
target_link_libraries(tinykv_cli PRIVATE tinykv)

enable_testing()
add_subdirectory(tests)
