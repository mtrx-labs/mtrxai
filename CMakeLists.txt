cmake_minimum_required(VERSION 3.20)
project(mtrxai VERSION 0.2.2 LANGUAGES CXX)

option(BUILD_TESTING "Build project tests" ON)
include(CTest)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "mtrxAI currently supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

option(MTRXAI_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
if(MTRXAI_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "MTRXAI_ENABLE_SANITIZERS requires GCC or Clang")
  endif()
endif()

# FTXUI is vendored so the normal application build does not need network access.
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(
  ftxui
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ftxui"
)
FetchContent_MakeAvailable(ftxui)

add_library(mtrxai_core STATIC
  src/cancel.cpp
  src/coder_transcript.cpp
  src/config.cpp
  src/endpoint.cpp
  src/files.cpp
  src/pipeline.cpp
  src/provider.cpp
  src/provider_stream.cpp
  src/safety.cpp
  src/session.cpp
  src/setup_wizard.cpp
  src/textutil.cpp
  src/signals.cpp
  src/tui.cpp
  src/update.cpp
)
target_include_directories(mtrxai_core PUBLIC include)
target_link_libraries(mtrxai_core PUBLIC
  CURL::libcurl
  OpenSSL::Crypto
  PkgConfig::JSONC
  ftxui::screen
  ftxui::dom
  ftxui::component
)
target_compile_definitions(mtrxai_core PUBLIC MTRXAI_VERSION="${PROJECT_VERSION}")
target_compile_options(mtrxai_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mtrxai src/main.cpp)
target_link_libraries(mtrxai PRIVATE mtrxai_core)
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
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  include(FetchContent)
  FetchContent_Declare(
    doctest
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest"
  )
  FetchContent_MakeAvailable(doctest)

  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE mtrxai_core doctest::doctest)
  target_compile_options(unit_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME unit_tests COMMAND unit_tests)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME pipeline-e2e
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline_e2e.py
              $<TARGET_FILE:mtrxai>)
  endif()
endif()
