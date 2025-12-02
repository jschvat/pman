cmake_minimum_required(VERSION 3.18)
project(pman LANGUAGES CXX VERSION 0.1.0)

option(PMAN_BUILD_EXAMPLES "Build example executables" ON)
option(PMAN_BUILD_TESTS "Build test targets" ON)

add_library(pman
    # Core thread management
    src/thread.cpp
    src/process.cpp
    src/thread_pool.cpp
    src/thread_group.cpp
    src/diagnostics.cpp
    src/topology.cpp
    src/thread_config.cpp
    src/numa.cpp
    src/scheduling.cpp
    src/futex.cpp
    src/pipe.cpp
    src/process_builder.cpp
    src/signal.cpp
    src/observability.cpp
    src/rt_memory.cpp

    # New synchronization primitives
    src/semaphore.cpp
    src/latch.cpp
    src/once.cpp
    src/rw_mutex.cpp
    src/recursive_mutex.cpp

    # Enhanced thread control
    src/pausable_thread.cpp
    src/cancellable_thread.cpp
    src/enhanced_thread_pool.cpp

    # Process group management
    src/process_group.cpp
    src/process_supervisor.cpp
    src/process_health.cpp

    # IPC
    src/shared_memory.cpp
    src/shared_sync.cpp
    src/message_queue.cpp
    src/unix_socket.cpp

    # Timing and scheduling
    src/timer_wheel.cpp
    src/periodic_scheduler.cpp
    src/cron_scheduler.cpp
    src/rate_limiter.cpp

    # Async (separate process working on this)
    src/async/event_loop.cpp
    src/async/epoll_loop.cpp
    src/async/io_uring_loop.cpp
    src/async/io_uring_syscalls.cpp
    src/async/task.cpp
    src/async/sleep.cpp
    src/async/timeout.cpp
    src/async/interval.cpp
    src/async/channel.cpp
)
add_library(pman::pman ALIAS pman)

target_include_directories(pman
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(pman PUBLIC cxx_std_20)
target_link_libraries(pman PUBLIC pthread rt)

if(PMAN_BUILD_EXAMPLES)
    add_executable(pman_example examples/basic_usage.cpp)
    target_link_libraries(pman_example PRIVATE pman)

    add_executable(pman_process_supervisor examples/process_supervisor.cpp)
    target_link_libraries(pman_process_supervisor PRIVATE pman)

    add_executable(pman_signal_monitor examples/signal_monitor.cpp)
    target_link_libraries(pman_signal_monitor PRIVATE pman)

    add_executable(pman_observability_snapshot examples/observability_snapshot.cpp)
    target_link_libraries(pman_observability_snapshot PRIVATE pman)

    add_executable(pman_structured_scope examples/structured_scope.cpp)
    target_link_libraries(pman_structured_scope PRIVATE pman)

    add_executable(pman_rt_memory_demo examples/rt_memory_demo.cpp)
    target_link_libraries(pman_rt_memory_demo PRIVATE pman)

    add_executable(pman_advanced_sync_demo examples/advanced_sync_demo.cpp)
    target_link_libraries(pman_advanced_sync_demo PRIVATE pman)

    add_executable(pman_benchmark_sync examples/benchmark_sync.cpp)
    target_link_libraries(pman_benchmark_sync PRIVATE pman)

    add_executable(pman_async_timer_demo examples/async_timer_demo.cpp)
    target_link_libraries(pman_async_timer_demo PRIVATE pman)

    add_executable(pman_async_simple_demo examples/async_simple_demo.cpp)
    target_link_libraries(pman_async_simple_demo PRIVATE pman)
endif()

if(PMAN_BUILD_TESTS)
    enable_testing()
    add_executable(pman_smoke tests/smoke.cpp)
    target_link_libraries(pman_smoke PRIVATE pman)
    add_test(NAME pman_smoke COMMAND pman_smoke)

    add_executable(pman_async_comprehensive tests/async_comprehensive.cpp)
    target_link_libraries(pman_async_comprehensive PRIVATE pman)
    add_test(NAME pman_async_comprehensive COMMAND pman_async_comprehensive)

    add_executable(pman_async_benchmark tests/async_benchmark.cpp)
    target_link_libraries(pman_async_benchmark PRIVATE pman)

    add_executable(pman_diagnostics tests/diagnostics.cpp)
    target_link_libraries(pman_diagnostics PRIVATE pman)
    add_test(NAME pman_diagnostics COMMAND pman_diagnostics)

    add_executable(pman_topology tests/topology.cpp)
    target_link_libraries(pman_topology PRIVATE pman)
    add_test(NAME pman_topology COMMAND pman_topology)

    add_executable(pman_thread_options tests/thread_options.cpp)
    target_link_libraries(pman_thread_options PRIVATE pman)
    add_test(NAME pman_thread_options COMMAND pman_thread_options)

    add_executable(pman_numa_allocator tests/numa_allocator.cpp)
    target_link_libraries(pman_numa_allocator PRIVATE pman)
    add_test(NAME pman_numa_allocator COMMAND pman_numa_allocator)

    add_executable(pman_scheduling tests/scheduling.cpp)
    target_link_libraries(pman_scheduling PRIVATE pman)
    add_test(NAME pman_scheduling COMMAND pman_scheduling)

    add_executable(pman_futex tests/futex.cpp)
    target_link_libraries(pman_futex PRIVATE pman pthread)
    add_test(NAME pman_futex COMMAND pman_futex)

    add_executable(pman_process_builder tests/process_builder.cpp)
    target_link_libraries(pman_process_builder PRIVATE pman)
    add_test(NAME pman_process_builder COMMAND pman_process_builder)

    add_executable(pman_signal_dispatch tests/signal_dispatch.cpp)
    target_link_libraries(pman_signal_dispatch PRIVATE pman pthread)
    add_test(NAME pman_signal_dispatch COMMAND pman_signal_dispatch)

    add_executable(pman_signal_grace tests/signal_grace.cpp)
    target_link_libraries(pman_signal_grace PRIVATE pman)
    add_test(NAME pman_signal_grace COMMAND pman_signal_grace)

    add_executable(pman_observability tests/observability.cpp)
    target_link_libraries(pman_observability PRIVATE pman)
    add_test(NAME pman_observability COMMAND pman_observability)

    add_executable(pman_structured tests/structured.cpp)
    target_link_libraries(pman_structured PRIVATE pman)
    add_test(NAME pman_structured COMMAND pman_structured)

    add_executable(pman_rt_memory tests/rt_memory.cpp)
    target_link_libraries(pman_rt_memory PRIVATE pman)
    add_test(NAME pman_rt_memory COMMAND pman_rt_memory)

    add_executable(pman_advanced_sync tests/advanced_sync.cpp)
    target_link_libraries(pman_advanced_sync PRIVATE pman)
    add_test(NAME pman_advanced_sync COMMAND pman_advanced_sync)

    # New comprehensive test suites
    add_executable(pman_sync_primitives tests/sync_primitives.cpp)
    target_link_libraries(pman_sync_primitives PRIVATE pman)
    add_test(NAME pman_sync_primitives COMMAND pman_sync_primitives)

    add_executable(pman_thread_control tests/thread_control.cpp)
    target_link_libraries(pman_thread_control PRIVATE pman)
    add_test(NAME pman_thread_control COMMAND pman_thread_control)

    add_executable(pman_enhanced_pool tests/enhanced_pool.cpp)
    target_link_libraries(pman_enhanced_pool PRIVATE pman)
    add_test(NAME pman_enhanced_pool COMMAND pman_enhanced_pool)

    add_executable(pman_process_management tests/process_management.cpp)
    target_link_libraries(pman_process_management PRIVATE pman)
    add_test(NAME pman_process_management COMMAND pman_process_management)

    add_executable(pman_ipc tests/ipc.cpp)
    target_link_libraries(pman_ipc PRIVATE pman)
    add_test(NAME pman_ipc COMMAND pman_ipc)

    add_executable(pman_timing_scheduling tests/timing_scheduling.cpp)
    target_link_libraries(pman_timing_scheduling PRIVATE pman)
    add_test(NAME pman_timing_scheduling COMMAND pman_timing_scheduling)
endif()

# Timeout demos
add_executable(pman_timeout_demo tests/timeout_demo.cpp)
target_link_libraries(pman_timeout_demo PRIVATE pman)

add_executable(pman_timeout_demo2 tests/timeout_demo2.cpp)
target_link_libraries(pman_timeout_demo2 PRIVATE pman)

add_executable(pman_timeout_comparison tests/timeout_comparison.cpp)
target_link_libraries(pman_timeout_comparison PRIVATE pman)

add_executable(pman_race_demo tests/race_demo.cpp)
target_link_libraries(pman_race_demo PRIVATE pman)
add_executable(pman_race_simple_test tests/race_simple_test.cpp)
target_link_libraries(pman_race_simple_test PRIVATE pman)
add_executable(test_runonce_debug tests/test_runonce_debug.cpp)
target_link_libraries(test_runonce_debug PRIVATE pman)

# Comprehensive test suites
add_executable(pman_comprehensive_stress tests/comprehensive_stress.cpp)
target_link_libraries(pman_comprehensive_stress PRIVATE pman rt)

add_executable(pman_performance_regression tests/performance_regression.cpp)
target_link_libraries(pman_performance_regression PRIVATE pman)

add_executable(pman_edge_cases tests/edge_cases.cpp)
target_link_libraries(pman_edge_cases PRIVATE pman)

add_executable(pman_io_uring_test tests/io_uring_test.cpp)
target_link_libraries(pman_io_uring_test PRIVATE pman)

add_executable(pman_combinators_test tests/combinators_test.cpp)
target_link_libraries(pman_combinators_test PRIVATE pman)

add_executable(pman_async_sync_primitives_test tests/async_sync_primitives_test.cpp)
target_link_libraries(pman_async_sync_primitives_test PRIVATE pman)

if(PMAN_BUILD_TESTS)
    add_test(NAME pman_io_uring_test COMMAND pman_io_uring_test)
    add_test(NAME pman_combinators_test COMMAND pman_combinators_test)
    add_test(NAME pman_async_sync_primitives_test COMMAND pman_async_sync_primitives_test)
endif()
