cmake_minimum_required(VERSION 3.16)
project(zzipp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(zzipp_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(zzipp_BUILD_TESTS "Build the test suite" ON)

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
target_compile_definitions(zzipp PRIVATE zzipp_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(zzipp_core PUBLIC /W4)
else()
  target_compile_options(zzipp_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

if(zzipp_SANITIZE AND NOT MSVC)
  target_compile_options(zzipp_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_libraries(zzipp_core PUBLIC -fsanitize=address,undefined)
endif()

install(TARGETS zzipp RUNTIME DESTINATION bin)

if(zzipp_BUILD_TESTS)
  enable_testing()
  add_executable(zzipp_tests tests/unit_tests.cpp)
  target_link_libraries(zzipp_tests PRIVATE zzipp_core)
  add_test(NAME unit COMMAND zzipp_tests)
  add_test(NAME roundtrip COMMAND ${CMAKE_SOURCE_DIR}/tests/roundtrip.sh $<TARGET_FILE:zzipp>)
endif()
