cmake_minimum_required(VERSION 3.16)
project(cagent C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CAGENT_SOURCES
  src/cagent.c
  src/runtime/event_bus.c
  src/runtime/scheduler.c
  src/runtime/state_machine.c
  src/runtime/policy_engine.c
  src/runtime/agent.c
  src/runtime/task.c
  src/cognition/reasoning.c
  src/cognition/planner.c
  src/cognition/evaluator.c
  src/cognition/blackboard.c
  src/cognition/attention.c
  src/memory/memory.c
  src/memory/kv.c
  src/memory/episode.c
  src/memory/vector.c
  src/memory/graph.c
  src/retrieval/engine.c
  src/retrieval/embedding.c
  src/retrieval/context_builder.c
  src/plugin_runtime/manager.c
  src/plugin_runtime/sandbox.c
  src/plugin_runtime/capability.c
  src/plugin_runtime/registry.c
  src/plugin_intelligence/analyzer.c
  src/plugin_intelligence/architect.c
  src/plugin_intelligence/codegen.c
  src/plugin_intelligence/testing.c
  src/plugin_intelligence/security.c
  src/llm/llm.c
  src/llm/openai.c
  src/llm/anthropic.c
  src/llm/mock.c
  src/llm/sse.c
  src/llm/router.c
  src/llm/usage.c
  src/action/tools.c
  src/action/tool_file.c
  src/action/tool_shell.c
  src/action/tool_git.c
  src/action/tool_mcp.c
  src/action/skill.c
  src/action/mcp_conn.c
  src/cluster/node.c
  src/tx/tx.c
  src/snapshot/snapshot.c
  src/snapshot/cow.c
  src/os/os_thread.c
  src/os/os_coro.c
  src/os/os_socket.c
  src/os/os_fs.c
  src/os/os_proc.c
  src/os/os_time.c
  src/os/http.c
  src/infra/logging.c
  src/infra/config.c
  src/infra/util.c
  src/infra/metrics.c
  src/infra/audit.c
  src/infra/persist.c
  src/infra/trace.c
  src/api/http_server.c
  src/api/api_rest.c
  src/api/auth.c
  src/api/websocket.c
  third_party/cJSON/cJSON.c
)

include_directories(include third_party/cJSON)

if(WIN32)
  set(PLATFORM_LIBS ws2_32)
else()
  set(PLATFORM_LIBS pthread dl)
endif()

add_executable(cagent ${CAGENT_SOURCES} cli/main.c)
target_link_libraries(cagent ${PLATFORM_LIBS})

add_executable(cagent-test ${CAGENT_SOURCES} tests/test_all.c)
target_link_libraries(cagent-test ${PLATFORM_LIBS})

add_executable(mock-llm-server ${CAGENT_SOURCES} tools/mock_llm_server.c)
target_link_libraries(mock-llm-server ${PLATFORM_LIBS})

add_executable(test-adapters ${CAGENT_SOURCES} tests/test_adapters.c)
target_link_libraries(test-adapters ${PLATFORM_LIBS})

add_executable(cagent-e2e ${CAGENT_SOURCES} tests/test_e2e.c)
target_link_libraries(cagent-e2e ${PLATFORM_LIBS})

add_executable(cagent-bench ${CAGENT_SOURCES} tests/bench_agent.c)
target_link_libraries(cagent-bench ${PLATFORM_LIBS})

add_executable(cagent-bench-real ${CAGENT_SOURCES} tests/bench_real.c)
target_link_libraries(cagent-bench-real ${PLATFORM_LIBS})
