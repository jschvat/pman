#!/usr/bin/env node

/**
 * Node.js async benchmark for comparison with pman async library
 *
 * Tests similar operations:
 * - Event loop overhead
 * - Timer operations
 * - Promise/async operations (equivalent to coroutines)
 * - Throughput
 */

function benchmark(name, fn) {
    const start = process.hrtime.bigint();
    const result = fn();

    // Handle async functions
    if (result instanceof Promise) {
        return result.then(() => {
            const end = process.hrtime.bigint();
            const microseconds = Number(end - start) / 1000;
            console.log(`${name.padEnd(40)} ${microseconds.toFixed(0).padStart(12)} μs`);
            return microseconds;
        });
    } else {
        const end = process.hrtime.bigint();
        const microseconds = Number(end - start) / 1000;
        console.log(`${name.padEnd(40)} ${microseconds.toFixed(0).padStart(12)} μs`);
        return microseconds;
    }
}

function separator() {
    console.log('-'.repeat(60));
}

async function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function main() {
    console.log('Node.js Async Performance Benchmarks');
    console.log('=====================================\n');

    // Benchmark 1: Timer creation and firing
    separator();
    console.log('Timer Operations:');
    separator();

    await benchmark('Add 1000 timers', () => {
        return new Promise(resolve => {
            let count = 0;
            for (let i = 0; i < 1000; i++) {
                setTimeout(() => {
                    count++;
                    if (count === 1000) resolve();
                }, 10);
            }
        });
    });

    await benchmark('Fire 100 immediate timers', () => {
        return new Promise(resolve => {
            let count = 0;
            for (let i = 0; i < 100; i++) {
                setImmediate(() => {
                    count++;
                    if (count === 100) resolve();
                });
            }
        });
    });

    await benchmark('Fire 1000 immediate timers', () => {
        return new Promise(resolve => {
            let count = 0;
            for (let i = 0; i < 1000; i++) {
                setImmediate(() => {
                    count++;
                    if (count === 1000) resolve();
                });
            }
        });
    });

    // Benchmark 2: Async/await operations (equivalent to coroutines)
    separator();
    console.log('\nAsync/Await Operations:');
    separator();

    await benchmark('Create 1000 async functions', () => {
        const promises = [];
        for (let i = 0; i < 1000; i++) {
            promises.push((async () => {})());
        }
        return Promise.all(promises);
    });

    await benchmark('Run 100 simple async functions', () => {
        return new Promise(resolve => {
            let count = 0;
            for (let i = 0; i < 100; i++) {
                (async () => {
                    await sleep(0);
                    count++;
                    if (count === 100) resolve();
                })();
            }
        });
    });

    await benchmark('Run 1000 simple async functions', () => {
        return new Promise(resolve => {
            let count = 0;
            for (let i = 0; i < 1000; i++) {
                (async () => {
                    await sleep(0);
                    count++;
                    if (count === 1000) resolve();
                })();
            }
        });
    });

    // Benchmark 3: Event loop iterations
    separator();
    console.log('\nEvent Loop Iterations:');
    separator();

    await benchmark('1000 setImmediate calls', () => {
        return new Promise(resolve => {
            let count = 0;
            function next() {
                count++;
                if (count < 1000) {
                    setImmediate(next);
                } else {
                    resolve();
                }
            }
            next();
        });
    });

    // Benchmark 4: Mixed workload
    separator();
    console.log('\nMixed Workload:');
    separator();

    await benchmark('100 timers + 100 async functions', () => {
        return new Promise(resolve => {
            let count = 0;
            const TARGET = 200;

            // Add timers
            for (let i = 0; i < 100; i++) {
                setTimeout(() => {
                    count++;
                    if (count === TARGET) resolve();
                }, i % 10);
            }

            // Add async functions
            for (let i = 0; i < 100; i++) {
                (async () => {
                    await sleep(i % 10);
                    count++;
                    if (count === TARGET) resolve();
                })();
            }
        });
    });

    // Benchmark 5: Throughput test
    separator();
    console.log('\nThroughput Tests:');
    separator();

    const throughput = await benchmark('Process 10,000 operations', () => {
        return new Promise(resolve => {
            let count = 0;
            const TARGET = 10000;

            for (let i = 0; i < TARGET; i++) {
                setImmediate(() => {
                    count++;
                    if (count === TARGET) resolve();
                });
            }
        });
    });

    console.log(`    Throughput: ${(10000 / throughput * 1000000).toFixed(2)} ops/sec`);

    // Benchmark 6: Promise creation overhead
    separator();
    console.log('\nPromise Creation Overhead:');
    separator();

    benchmark('Create 10,000 resolved promises', () => {
        for (let i = 0; i < 10000; i++) {
            Promise.resolve();
        }
    });

    await benchmark('Await 10,000 resolved promises', async () => {
        const promises = [];
        for (let i = 0; i < 10000; i++) {
            promises.push(Promise.resolve());
        }
        await Promise.all(promises);
    });

    // Benchmark 7: Memory and GC impact
    separator();
    console.log('\nMemory Info:');
    separator();

    const mem = process.memoryUsage();
    console.log(`Heap Used:     ${(mem.heapUsed / 1024 / 1024).toFixed(2)} MB`);
    console.log(`Heap Total:    ${(mem.heapTotal / 1024 / 1024).toFixed(2)} MB`);
    console.log(`RSS:           ${(mem.rss / 1024 / 1024).toFixed(2)} MB`);
    console.log(`External:      ${(mem.external / 1024 / 1024).toFixed(2)} MB`);

    separator();
    console.log('\nBenchmark complete!');
}

main().catch(console.error);
