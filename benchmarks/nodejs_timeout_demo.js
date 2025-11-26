#!/usr/bin/env node

/**
 * Node.js timeout demo - equivalent to the C++ version
 * Shows whether tasks continue after timeout
 */

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

// Timeout wrapper function (equivalent to C++ timeout())
function timeout(promise, ms) {
    return Promise.race([
        promise,
        new Promise((_, reject) =>
            setTimeout(() => reject(new Error('TIMEOUT')), ms)
        )
    ]);
}

// Scenario 1: Task wrapped in timeout - does it stop?
async function slowTask() {
    console.log('[Wrapped Task] Starting...');

    for (let i = 1; i <= 10; i++) {
        console.log(`[Wrapped Task] Iteration ${i}/10`);
        await sleep(1000);
    }

    console.log('[Wrapped Task] Completed!');
    return 42;
}

async function scenario1() {
    console.log('\n=== SCENARIO 1: Task wrapped in timeout() ===\n');

    try {
        const result = await timeout(slowTask(), 3000);
        console.log(`[Main] Task completed with result: ${result}`);
    } catch (e) {
        console.log('[Main] TIMEOUT at 3 seconds!');
        console.log('[Main] Question: Did the task stop?');
    }

    console.log('\n[Main] Waiting 8 more seconds to observe...');
    await sleep(8000);

    console.log('[Main] Demo complete!\n');
}

// Scenario 2: What if we DON'T await?
async function independentTask() {
    console.log('[Independent Task] Starting...');

    for (let i = 1; i <= 10; i++) {
        console.log(`[Independent Task] Iteration ${i}/10`);
        await sleep(1000);
    }

    console.log('[Independent Task] Completed!');
}

async function scenario2() {
    console.log('\n=== SCENARIO 2: Task started without await ===\n');

    // Start task but DON'T await it
    independentTask(); // Fire and forget

    console.log('[Main] Started independent task, waiting 3 seconds...');
    await sleep(3000);

    console.log('[Main] Main done waiting');
    console.log('[Main] Does independent task continue?\n');

    console.log('[Main] Waiting 8 more seconds...');
    await sleep(8000);

    console.log('[Main] Demo complete!\n');
}

// Scenario 3: Using AbortController (proper cancellation)
async function cancellableTask(signal) {
    console.log('[Cancellable Task] Starting...');

    for (let i = 1; i <= 10; i++) {
        if (signal.aborted) {
            console.log('[Cancellable Task] CANCELLED!');
            throw new Error('Cancelled');
        }
        console.log(`[Cancellable Task] Iteration ${i}/10`);
        await sleep(1000);
    }

    console.log('[Cancellable Task] Completed!');
    return 42;
}

async function scenario3() {
    console.log('\n=== SCENARIO 3: Using AbortController (proper cancellation) ===\n');

    const controller = new AbortController();

    // Set timeout that aborts
    const timeoutId = setTimeout(() => {
        console.log('[Main] Timeout reached - aborting task!');
        controller.abort();
    }, 3000);

    try {
        const result = await cancellableTask(controller.signal);
        console.log(`[Main] Task completed with result: ${result}`);
        clearTimeout(timeoutId);
    } catch (e) {
        console.log(`[Main] Task was cancelled: ${e.message}`);
    }

    console.log('\n[Main] Waiting 3 more seconds...');
    await sleep(3000);

    console.log('[Main] Demo complete!\n');
}

// Main
async function main() {
    const scenario = process.argv[2] || '0';

    console.log('\n========================================');
    console.log('Node.js Timeout Behavior Demo');
    console.log('========================================');

    switch (scenario) {
        case '1':
            await scenario1();
            break;
        case '2':
            await scenario2();
            break;
        case '3':
            await scenario3();
            break;
        default:
            console.log('\nUsage:');
            console.log('  node nodejs_timeout_demo.js 1  - Show if timeout() stops task');
            console.log('  node nodejs_timeout_demo.js 2  - Show independent task behavior');
            console.log('  node nodejs_timeout_demo.js 3  - Show proper cancellation\n');
            return;
    }

    console.log('========================================\n');
}

main().catch(console.error);
