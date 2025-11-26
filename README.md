# pman

Modern C++ utilities that extend the ergonomics of Linux thread and process management.  The library layers RAII helpers and higher–level patterns on top of `pthread`/`fork` primitives without hiding the power of the underlying system calls.

## Features

- `ManagedThread`: deterministic ownership of `pthread_t` plus helpers for affinity, scheduling policy, and exception propagation.
- `ThreadGroup`: coordinate a set of managed threads (bulk join/detach, fan-out affinity changes).
- `ThreadPool`: lightweight task scheduler that reuses `ManagedThread` workers.
- `ProcessHandle`: ergonomic wrapper around Linux `fork/exec` with post-launch controls (signals, priorities, affinity).
- `SystemTopology`: NUMA-aware CPU discovery that powers node-level placement helpers across the library.
- `ThreadOptions`: configure existing threads (affinity masks, core siblings, FIFO priority, memory locking) without recreating them.
- `NumaAllocator` + `ScopedMemoryPolicy`: lightweight primitives to bias memory allocations toward specific NUMA nodes without external dependencies.
- `Scheduling` helpers: precise sleep, timer slack control, FIFO/RR convenience setters, and an RAII deadline scope (SCHED_DEADLINE) for deterministic workloads.
- `ProcessBuilder`: race-free child management with pidfds, optional stdio pipes, parent-death signals, cgroup/ionice hooks, and optional `posix_spawn`-based launches for fork-free spawns.
- `SignalDispatcher`: centralize POSIX signals via `signalfd`, block them in worker threads, and trigger built-in grace sequences (`gracefulShutdown`, `sendSignalSequence`) for TERM→INT→HUP→KILL escalation without bespoke plumbing.
- `Observability` snapshots: pull `/proc`-style metrics (utime/stime, context switches, per-thread metadata) without sprinkling bespoke parsers throughout the codebase.
- `TaskScope` + `with_deadline`: structured concurrency atop `std::stop_token`, cooperative cancellation, and easy deadline-based cancellation helpers.
- `RT memory controls`: lock working sets with `mlockall`, prefault stacks, toggle guard pages, and opt into `madvise(MADV_WILLNEED/HUGEPAGE)` through `pman::rt_memory` helpers.
- `Advanced sync`: futex mutex/condvar plus seqlocks, RW spin locks, and barriers for low-latency coordination.

## Building

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Set `PMAN_BUILD_EXAMPLES=ON` (default) to compile `pman_example`, a short interactive demonstration under `examples/`.

## Usage snapshot

```cpp
#include "pman/thread_pool.hpp"

pman::ThreadPool pool{4};
pool.submitTask([] { /* background work */ });

pman::CpuSet cpus = pman::CpuSet::range(0, 2);
pool.setAffinity(cpus);  // pin pool to CPU0-1

const auto& topo = pman::SystemTopology::instance();
if (!topo.nodes().empty()) {
    pool.pinWorkersToNode(topo.nodes().front().nodeId);  // NUMA-aware placement
}

pman::ThreadOptions opts;
opts.cpus = std::vector<int>{topo.cpuIds().front()};
opts.schedFifoPriority = 10;
pman::configure_current_thread(opts);  // elevate and pin caller

pman::ProcessBuilder builder{"/bin/echo"};
builder.arg("hello world");
auto pipe = builder.captureStdout();
auto child = builder.spawn();
child.wait();  // pidfd-backed wait
// pipe.readEnd() now holds the child's stdout for ingestion

auto snap = pman::snapshotProcess();
for (const auto& thread : snap.threads) {
    // inspect thread.userTimeTicks / thread.state
}

## Examples

All executables live under `examples/` (built when `PMAN_BUILD_EXAMPLES=ON`):

- `basic_usage.cpp` – high-level tour that wires up a `ThreadPool`, `ThreadGroup`, and `ProcessHandle` to show the core primitives together.
- `process_supervisor.cpp` – captures `/bin/echo` output via `ProcessBuilder`, then launches `/bin/sleep` and terminates it after a graceful timeout to demonstrate pidfd-backed shutdown.
- `signal_monitor.cpp` – enables `SignalDispatcher` for `SIGUSR1`, blocks signals in the main thread, and shows how callbacks run via `signalfd` plus how to emit grace sequences.
- `observability_snapshot.cpp` – starts a couple of worker threads and prints the `/proc`-derived metrics returned by `snapshotProcess()` so you can see thread states, utime/stime, and context switches in practice.
- `structured_scope.cpp` – spawns cancellable tasks with `TaskScope` and demonstrates `with_deadline`-based stop tokens.
- `rt_memory_demo.cpp` – walks through `pman::rt_memory` helpers (mlockall, madvise, stack prefaulting, guard sizes).
- `advanced_sync_demo.cpp` – showcases the seqlock, RW spin lock, and barrier utilities in action.
- `benchmark_sync.cpp` – quick-and-dirty comparison between `pman::SeqLock` and `std::shared_mutex` under read-heavy load.
```

See `examples/basic_usage.cpp` for a more complete walk through that mixes thread pools, process control, and affinity tweaks.
