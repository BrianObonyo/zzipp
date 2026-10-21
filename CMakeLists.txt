cmake_minimum_required(VERSION 3.16)
project(zzipp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ZZIPP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ZZIPP_BUILD_TESTS "Build the test suite" ON)
option(ZZIPP_BUILD_GUI "Build the Qt 6 desktop app (skipped automatically if Qt 6 is not found)" ON)

find_package(Threads REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)

add_library(zzipp_core STATIC
  src/archive.cpp src/classify.cpp src/codec.cpp src/create.cpp
  src/extract.cpp src/format.cpp src/util.cpp src/zip_export.cpp)
target_include_directories(zzipp_core PUBLIC src ${LIBLZMA_INCLUDE_DIRS})
target_link_libraries(zzipp_core PUBLIC ${LIBLZMA_LIBRARIES} ZLIB::ZLIB Threads::Threads)

add_executable(zzipp src/main.cpp)
target_link_libraries(zzipp PRIVATE zzipp_core)
target_compile_definitions(zzipp PRIVATE ZZIPP_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(zzipp_core PUBLIC /W4)
else()
  target_compile_options(zzipp_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

if(ZZIPP_SANITIZE AND NOT MSVC)
  target_compile_options(zzipp_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_libraries(zzipp_core PUBLIC -fsanitize=address,undefined)
endif()

install(TARGETS zzipp RUNTIME DESTINATION bin)

if(ZZIPP_BUILD_TESTS)
  enable_testing()
  add_executable(zzipp_tests tests/unit_tests.cpp)
  target_link_libraries(zzipp_tests PRIVATE zzipp_core)
  add_test(NAME unit COMMAND zzipp_tests)
  add_test(NAME roundtrip COMMAND ${CMAKE_SOURCE_DIR}/tests/roundtrip.sh $<TARGET_FILE:zzipp>)
  add_test(NAME edge_cases COMMAND ${CMAKE_SOURCE_DIR}/tests/edge_cases.sh $<TARGET_FILE:zzipp>)
endif()

if(ZZIPP_BUILD_GUI)
  find_package(Qt6 6.2 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    add_subdirectory(src/gui)
  else()
    message(STATUS "Qt 6 not found: building the command-line tool only")
  endif()
endif()
