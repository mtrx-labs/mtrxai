cmake_minimum_required(VERSION 3.20)
project(mtrxai VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "mtrxAI currently supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(Curses REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_executable(mtrxai
  src/config.cpp
  src/files.cpp
  src/main.cpp
  src/pipeline.cpp
  src/provider.cpp
  src/tui.cpp
)
target_include_directories(mtrxai PRIVATE include ${CURSES_INCLUDE_DIRS})
target_link_libraries(mtrxai PRIVATE CURL::libcurl ${CURSES_LIBRARIES} PkgConfig::JSONC)
target_compile_options(mtrxai PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS mtrxai RUNTIME DESTINATION bin)
