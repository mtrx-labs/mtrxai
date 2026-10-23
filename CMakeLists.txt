cmake_minimum_required(VERSION 3.20)
project(mtrxai VERSION 0.2.1 LANGUAGES CXX)

include(CTest)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "mtrxAI currently supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(Curses REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_executable(mtrxai
  src/config.cpp
  src/files.cpp
  src/main.cpp
  src/pipeline.cpp
  src/provider.cpp
  src/setup_wizard.cpp
  src/tui.cpp
  src/update.cpp
)
target_include_directories(mtrxai PRIVATE include ${CURSES_INCLUDE_DIRS})
target_link_libraries(mtrxai PRIVATE CURL::libcurl OpenSSL::Crypto ${CURSES_LIBRARIES} PkgConfig::JSONC)
target_compile_definitions(mtrxai PRIVATE MTRXAI_VERSION="${PROJECT_VERSION}")
target_compile_options(mtrxai PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS mtrxai RUNTIME DESTINATION bin)
install(FILES LICENSE DESTINATION share/licenses/mtrxai)
install(FILES README.md DESTINATION share/doc/mtrxai)

set(CPACK_PACKAGE_NAME "mtrxai")
set(CPACK_PACKAGE_VENDOR "mtrx-labs")
set(CPACK_PACKAGE_CONTACT "mtrx-labs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Sequential, low-memory AI coding assistant for Linux")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mtrx-labs/mtrxai")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "DEB;RPM")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "mtrx-labs")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
include(CPack)

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME pipeline-e2e
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline_e2e.py
              $<TARGET_FILE:mtrxai>)
  endif()
endif()
