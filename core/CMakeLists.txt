cmake_minimum_required(VERSION 3.20)
project(tunegram_lt_core VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LT_CORE_NO_EXCEPTIONS "Build the core library without exceptions/RTTI (smaller, mobile-friendly)" ON)
option(LT_CORE_BUILD_TESTS "Build simulation tests" ON)
option(LT_CORE_BUILD_TOOLS "Build lt_stdio (e2e bridge) and the size probe" ON)
option(LT_CORE_SANITIZE "Build tests with ASan+UBSan" OFF)

set(LT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# The session core: a small static library with a C API.
add_library(lt_core STATIC
  src/json.cpp
  src/protocol.cpp
  src/reducer.cpp
  src/session.cpp
  src/c_api.cpp
)
target_include_directories(lt_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(lt_core PRIVATE ${LT_WARNINGS} -ffunction-sections -fdata-sections -fvisibility=hidden
  -fvisibility-inlines-hidden)
if(LT_CORE_NO_EXCEPTIONS)
  target_compile_options(lt_core PRIVATE -fno-exceptions -fno-rtti)
endif()
if(LT_CORE_SANITIZE)
  target_compile_options(lt_core PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lt_core INTERFACE -fsanitize=address,undefined)
endif()
# Internals are hidden; the C API is marked LT_API (default visibility) in the header.
set_target_properties(lt_core PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)

# ---------------------------------------------------------------------------
if(LT_CORE_BUILD_TESTS)
  enable_testing()
  add_executable(lt_tests
    tests/main.cpp
    tests/sim.cpp
    tests/test_json.cpp
    tests/test_reducer.cpp
    tests/test_session.cpp
    tests/test_convergence.cpp
  )
  target_include_directories(lt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(lt_tests PRIVATE lt_core)
  target_compile_options(lt_tests PRIVATE ${LT_WARNINGS})
  if(LT_CORE_SANITIZE)
    target_compile_options(lt_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lt_tests PRIVATE -fsanitize=address,undefined)
  endif()

  # One ctest entry per test group so `ctest -j` runs them in parallel.
  foreach(group json reducer clock local_first reanchor correction join reconnect
                navigation shuffle repeat cap version idempotent advance convergence capi interrupt leave)
    add_test(NAME ${group} COMMAND lt_tests ${group})
  endforeach()
endif()

if(LT_CORE_BUILD_TOOLS)
  add_executable(lt_stdio tools/lt_stdio.cpp)
  target_include_directories(lt_stdio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(lt_stdio PRIVATE lt_core)
  target_compile_options(lt_stdio PRIVATE ${LT_WARNINGS})

  # Links every C API function into a tiny C program to measure what the core
  # adds to an app binary.
  add_executable(lt_size_probe tools/size_probe.c)
  target_link_libraries(lt_size_probe PRIVATE lt_core)
  target_link_options(lt_size_probe PRIVATE $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip> $<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,--gc-sections>)
  add_executable(lt_size_empty tools/size_empty.c)
endif()
