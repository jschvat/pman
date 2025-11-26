# Throughput Optimization Strategy

## Current Performance
- **PMAN**: 560K ops/sec
- **Node.js**: 2.3M ops/sec
- **Gap**: 4.1x slower

## Identified Bottlenecks

### 1. Timer Processing (Biggest Impact)
**Problem**: `processTimers()` called every iteration, even when no timers ready
```cpp
void runOnce() {
    epoll_wait(...);
    processTimers();  // Called ALWAYS, even if next timer is hours away!
    // ...
}
```

**Solution**: Calculate timeout dynamically, skip processing if no timers ready
```cpp
auto timeout = getNextTimerTimeout();  // Only wait until next timer
epoll_wait(..., timeout);
if (hasExpiredTimers()) processTimers();  // Only process if needed
```

**Expected Gain**: 2-3x improvement

### 2. Priority Queue Overhead
**Problem**: `std::priority_queue` requires O(log n) rebuild on each pop
- Can't peek at second element without popping
- Can't efficiently batch process multiple expired timers

**Solution**: Use `std::multimap` or custom min-heap
```cpp
std::multimap<TimePoint, uint64_t> timersByExpiry_;
// OR
std::vector<TimerData> heap_;  // Custom heap with batch processing
```

**Expected Gain**: 20-30% improvement for many timers

### 3. No Event Batching
**Problem**: Processes one callback per loop iteration
```cpp
while (!timers_.empty()) {
    auto timer = timers_.top();
    // ... process ONE timer ...
    callback();  // May schedule more work, but we continue loop
}
```

**Solution**: Batch callbacks, process all at once
```cpp
std::vector<std::function<void()>> batch;
while (!timers_.empty() && timer.expiry <= now) {
    batch.push_back(callback);  // Collect
}
for (auto& cb : batch) cb();  // Execute batch
```

**Expected Gain**: 15-25% improvement

### 4. Unnecessary Syscalls
**Problem**: Writing to eventfd on every `addTimer()`
```cpp
uint64_t addTimer(...) {
    // ... add timer ...
    uint64_t val = 1;
    write(eventFd_, &val, sizeof(val));  // Syscall on EVERY timer!
    return id;
}
```

**Solution**: Only wake if epoll is blocking
```cpp
uint64_t addTimer(...) {
    // ... add timer ...
    if (isBlocking_) {
        write(eventFd_, &val, sizeof(val));  // Only when needed
    }
    return id;
}
```

**Expected Gain**: 30-40% improvement

### 5. Map Lookup Overhead
**Problem**: Multiple map lookups per operation
```cpp
auto callbackIt = timerCallbacks_.find(timerId);
auto periodIt = timerPeriods_.find(timerId);
```

**Solution**: Store callback with timer data
```cpp
struct TimerEntry {
    uint64_t id;
    TimePoint expiry;
    std::function<void()> callback;
    std::optional<Duration> period;
};
```

**Expected Gain**: 10-15% improvement

## Implementation Priority

### Phase 1: Quick Wins (30min)
1. ✅ Skip processTimers() when no expired timers
2. ✅ Remove unnecessary eventfd writes
3. ✅ Store callbacks with timer data (eliminate map lookups)

**Expected**: 2x improvement → ~1.1M ops/sec

### Phase 2: Structural Changes (2hrs)
1. Replace priority_queue with multimap or custom heap
2. Implement callback batching
3. Add timer coalescing (group nearby timers)

**Expected**: 3x improvement → ~1.7M ops/sec

### Phase 3: Advanced (4hrs)
1. Phase-based event loop (like Node.js)
2. Lock-free data structures for posted callbacks
3. Timer wheel for high-frequency timers
4. SIMD optimizations for batch operations

**Expected**: 4x improvement → ~2.2M ops/sec (approaching Node.js)

## Benchmark Targets

```
Current:      560K ops/sec
Phase 1:    1,100K ops/sec  (2x)
Phase 2:    1,700K ops/sec  (3x)
Phase 3:    2,200K ops/sec  (4x - matching Node.js)
```

## Trade-offs

### Memory vs Speed
- Storing callbacks with timers: +8 bytes per timer
- Custom heap: Better cache locality, slightly more memory
- **Verdict**: Worth it - memory is cheap, speed matters

### Latency vs Throughput
- Batching adds ~0-100μs latency
- But increases throughput 2-3x
- **Verdict**: Acceptable for most use cases, keep current behavior for real-time

### Complexity vs Maintainability
- Custom heap: More code, harder to debug
- But well-tested pattern (libuv uses similar)
- **Verdict**: Worth it if properly tested

## Testing Strategy

1. **Correctness**: All existing tests must pass
2. **Performance**: Benchmark each optimization
3. **Regression**: Ensure latency doesn't degrade
4. **Memory**: Monitor memory usage under load

## Implementation

Start with Phase 1 (quick wins), measure, then decide on Phase 2/3 based on needs.
