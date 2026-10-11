cmake_minimum_required(VERSION 3.16)

# TinyPPP: build and test the PPP engine on the desktop with the
# Arduino-Emulator (https://github.com/pschatzmann/Arduino-Emulator).
#
#   cmake -B build && cmake --build build && ctest --test-dir build
#
# Use a local emulator checkout instead of downloading it:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_ARDUINO_EMULATOR=~/Development/Arduino-Emulator
project(TinyPPP LANGUAGES C CXX)

option(TINYPPP_BUILD_TESTS "Build the unit tests" ON)
option(TINYPPP_BUILD_EXAMPLES "Build the desktop examples" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(arduino_emulator
  GIT_REPOSITORY "https://github.com/pschatzmann/Arduino-Emulator.git"
  GIT_TAG main)
FetchContent_GetProperties(arduino_emulator)
if(NOT arduino_emulator_POPULATED)
  FetchContent_Populate(arduino_emulator)
  add_subdirectory(${arduino_emulator_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/arduino_emulator)
endif()

# treat the emulator headers as system headers: -Wall -Wextra only reports
# warnings in TinyPPP
get_target_property(_emulator_includes arduino_emulator INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET arduino_emulator APPEND PROPERTY
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_emulator_includes})

# header only library
add_library(tinyppp INTERFACE)
target_include_directories(tinyppp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tinyppp INTERFACE arduino_emulator)

if(TINYPPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TINYPPP_BUILD_EXAMPLES AND UNIX)
  # arduino_sketch(): builds an Arduino sketch with the emulator
  include(${arduino_emulator_SOURCE_DIR}/Arduino.cmake)
  add_subdirectory(examples/desktop)
endif()
