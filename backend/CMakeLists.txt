cmake_minimum_required(VERSION 3.16)
project(cognitive-os-agent C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/include/api/web_ui.h
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_web_ui.py
  DEPENDS apps/web/index.html tools/gen_web_ui.py VERBATIM)
add_custom_target(web-ui DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/api/web_ui.h)
file(GLOB_RECURSE CAGENT_SOURCES CONFIGURE_DEPENDS src/*.c)
list(FILTER CAGENT_SOURCES EXCLUDE REGEX "/os/(windows|linux|macos|posix)/")
if(WIN32)
  file(GLOB PLATFORM_SRCS CONFIGURE_DEPENDS src/os/windows/*.c)
  set(PLATFORM_LIBS ws2_32 winhttp bcrypt m)
else()
  file(GLOB PLATFORM_SRCS CONFIGURE_DEPENDS src/os/posix/*.c)
  if(APPLE)
    list(APPEND PLATFORM_SRCS src/os/macos/http.c)
  else()
    list(APPEND PLATFORM_SRCS src/os/linux/http.c)
  endif()
  find_package(Threads REQUIRED)
  set(PLATFORM_LIBS Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
list(APPEND CAGENT_SOURCES ${PLATFORM_SRCS} third_party/cJSON/cJSON.c third_party/wasm3/wasm3_all.c)
add_library(cagent STATIC ${CAGENT_SOURCES})
add_dependencies(cagent web-ui)
target_include_directories(cagent PUBLIC include third_party/cJSON third_party/wasm3)
target_link_libraries(cagent PUBLIC ${PLATFORM_LIBS})

add_executable(cognitive-os-agent cli/main.c)
target_link_libraries(cognitive-os-agent cagent)

add_executable(cognitive-os-agent-test tests/test_all.c)
target_link_libraries(cognitive-os-agent-test cagent)

add_executable(mock-llm-server tools/mock_llm_server.c)
target_link_libraries(mock-llm-server cagent)

add_executable(test-adapters tests/test_adapters.c)
target_link_libraries(test-adapters cagent)

add_executable(cognitive-os-agent-e2e tests/test_e2e.c)
target_link_libraries(cognitive-os-agent-e2e cagent)

add_executable(cognitive-os-agent-bench tests/bench_agent.c)
target_link_libraries(cognitive-os-agent-bench cagent)

add_executable(cognitive-os-agent-bench-real tests/bench_real.c)
target_link_libraries(cognitive-os-agent-bench-real cagent)

add_executable(cognitive-os-agent-scenario tests/test_scenario.c)
target_link_libraries(cognitive-os-agent-scenario cagent)
enable_testing()
add_test(NAME unit COMMAND cognitive-os-agent-test)
add_test(NAME scenario COMMAND cognitive-os-agent-scenario)
add_test(NAME benchmark COMMAND cognitive-os-agent-bench --mock)
# Tests spawn repository fixtures such as tools/mock_mcp_server.js and use
# source-relative state paths. Multi-config/out-of-tree builds (including the
# macOS CI build-macos directory) must therefore run from the source root.
set_tests_properties(unit scenario benchmark PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
