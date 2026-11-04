#include "eventqueue.h"

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <map>

using namespace Natsuyume;

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while (0)

// ---------------------------------------------------------------------------
// Test 1: FIFO Ordering
// ---------------------------------------------------------------------------
static void test_fifo_ordering()
{
    std::cout << "[Test 1] FIFO Ordering..." << std::flush;
    EventQueue q;
    TEST_ASSERT(q.isValid());

    TEST_ASSERT(q.post(Event::makeState(true)));
    TEST_ASSERT(q.post(Event::makeCustom(101, "first")));
    TEST_ASSERT(q.post(Event::makeDuration(42000)));
    TEST_ASSERT(q.post(Event::makeTrackChanged(7)));
    TEST_ASSERT(q.post(Event::makeEndOfStream()));

    std::vector<Event> received;
    size_t count = q.pump([&](const Event &ev) {
        received.push_back(ev);
    });

    TEST_ASSERT(count == 5);
    TEST_ASSERT(received.size() == 5);

    TEST_ASSERT(received[0].type == EventType::StateChanged);
    TEST_ASSERT(std::get<StateChangedPayload>(received[0].payload).isPlaying == true);

    TEST_ASSERT(received[1].type == EventType::Custom);
    TEST_ASSERT(std::get<CustomPayload>(received[1].payload).code == 101);
    TEST_ASSERT(std::get<CustomPayload>(received[1].payload).text == "first");

    TEST_ASSERT(received[2].type == EventType::DurationChanged);
    TEST_ASSERT(std::get<DurationChangedPayload>(received[2].payload).durationMs == 42000);

    TEST_ASSERT(received[3].type == EventType::TrackChanged);
    TEST_ASSERT(std::get<TrackChangedPayload>(received[3].payload).trackId == 7);

    TEST_ASSERT(received[4].type == EventType::EndOfStream);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 2: Concurrent Producers
// ---------------------------------------------------------------------------
static void test_concurrent_producers()
{
    std::cout << "[Test 2] Concurrent Producers..." << std::flush;
    EventQueue q;
    const int kNumThreads = 8;
    const int kEventsPerThread = 500;

    std::vector<std::thread> producers;
    producers.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        producers.emplace_back([&q, t]() {
            for (int i = 0; i < kEventsPerThread; ++i) {
                int64_t code = (static_cast<int64_t>(t) << 32) | static_cast<int64_t>(i);
                bool ok = q.post(Event::makeCustom(code));
                TEST_ASSERT(ok);
            }
        });
    }

    for (auto &t : producers) {
        t.join();
    }

    std::vector<Event> allEvents;
    q.drain(allEvents);

    TEST_ASSERT(allEvents.size() == static_cast<size_t>(kNumThreads * kEventsPerThread));

    // Verify per-thread monotonic ordering
    std::map<int, int> nextExpectedIndex;
    for (int t = 0; t < kNumThreads; ++t) {
        nextExpectedIndex[t] = 0;
    }

    for (const auto &ev : allEvents) {
        TEST_ASSERT(ev.type == EventType::Custom);
        int64_t code = std::get<CustomPayload>(ev.payload).code;
        int threadId = static_cast<int>(code >> 32);
        int seq = static_cast<int>(code & 0xFFFFFFFF);

        TEST_ASSERT(threadId >= 0 && threadId < kNumThreads);
        TEST_ASSERT(seq == nextExpectedIndex[threadId]);
        nextExpectedIndex[threadId]++;
    }

    for (int t = 0; t < kNumThreads; ++t) {
        TEST_ASSERT(nextExpectedIndex[t] == kEventsPerThread);
    }

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 3: Single-Consumer Batch Retrieval
// ---------------------------------------------------------------------------
static void test_single_consumer_batch_retrieval()
{
    std::cout << "[Test 3] Single-Consumer Batch Retrieval..." << std::flush;
    EventQueue q;

    for (int i = 0; i < 20; ++i) {
        q.post(Event::makeCustom(i));
    }

    std::vector<Event> batch;
    size_t drained = q.drain(batch);

    TEST_ASSERT(drained == 20);
    TEST_ASSERT(batch.size() == 20);
    TEST_ASSERT(q.size() == 0);

    for (int i = 0; i < 20; ++i) {
        TEST_ASSERT(std::get<CustomPayload>(batch[i].payload).code == i);
    }

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 4: Consecutive PositionChanged Coalescing
// ---------------------------------------------------------------------------
static void test_consecutive_position_coalescing()
{
    std::cout << "[Test 4] Consecutive PositionChanged Coalescing..." << std::flush;
    EventQueue q;

    TEST_ASSERT(q.post(Event::makePosition(100)));
    TEST_ASSERT(q.post(Event::makePosition(200)));
    TEST_ASSERT(q.post(Event::makePosition(300)));
    TEST_ASSERT(q.post(Event::makePosition(400)));
    TEST_ASSERT(q.post(Event::makePosition(500)));

    std::vector<Event> batch;
    size_t count = q.drain(batch);

    TEST_ASSERT(count == 1);
    TEST_ASSERT(batch.size() == 1);
    TEST_ASSERT(batch[0].type == EventType::PositionChanged);
    TEST_ASSERT(std::get<PositionChangedPayload>(batch[0].payload).positionMs == 500);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 5: PositionChanged Events Separated by Lifecycle Events
// ---------------------------------------------------------------------------
static void test_position_separated_by_lifecycle()
{
    std::cout << "[Test 5] Position Separated by Lifecycle Events..." << std::flush;
    EventQueue q;

    q.post(Event::makePosition(100));
    q.post(Event::makePosition(200));
    q.post(Event::makeState(false)); // Lifecycle event
    q.post(Event::makePosition(300));
    q.post(Event::makePosition(400));

    std::vector<Event> batch;
    q.drain(batch);

    // Expected: Position(200) [coalesced], State(false), Position(400) [coalesced]
    TEST_ASSERT(batch.size() == 3);

    TEST_ASSERT(batch[0].type == EventType::PositionChanged);
    TEST_ASSERT(std::get<PositionChangedPayload>(batch[0].payload).positionMs == 200);

    TEST_ASSERT(batch[1].type == EventType::StateChanged);
    TEST_ASSERT(std::get<StateChangedPayload>(batch[1].payload).isPlaying == false);

    TEST_ASSERT(batch[2].type == EventType::PositionChanged);
    TEST_ASSERT(std::get<PositionChangedPayload>(batch[2].payload).positionMs == 400);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 6: Drain-then-Swap Wakeup Correctness
// ---------------------------------------------------------------------------
static void test_drain_then_swap_wakeup()
{
    std::cout << "[Test 6] Drain-then-swap Wakeup Correctness..." << std::flush;
    EventQueue q;
    int pfd = q.wakeupFd();
    TEST_ASSERT(pfd >= 0);

    // Before posting: poll should timeout immediately
    struct pollfd fds = { pfd, POLLIN, 0 };
    int ret = ::poll(&fds, 1, 0);
    TEST_ASSERT(ret == 0);

    // Post event: poll should immediately return 1
    q.post(Event::makeCustom(1));
    ret = ::poll(&fds, 1, 100);
    TEST_ASSERT(ret == 1);
    TEST_ASSERT(fds.revents & POLLIN);

    // Drain: descriptor must be drained and no longer readable
    std::vector<Event> batch;
    q.drain(batch);
    TEST_ASSERT(batch.size() == 1);

    fds.revents = 0;
    ret = ::poll(&fds, 1, 0);
    TEST_ASSERT(ret == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 7: Empty Queue Pumping
// ---------------------------------------------------------------------------
static void test_empty_queue_pumping()
{
    std::cout << "[Test 7] Empty Queue Pumping..." << std::flush;
    EventQueue q;

    size_t count = q.pump([](const Event &) {
        TEST_ASSERT(false && "Should not be called for empty queue");
    });
    TEST_ASSERT(count == 0);

    std::vector<Event> batch;
    size_t drained = q.drain(batch);
    TEST_ASSERT(drained == 0);
    TEST_ASSERT(batch.empty());

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 8: Posting After Closure
// ---------------------------------------------------------------------------
static void test_posting_after_closure()
{
    std::cout << "[Test 8] Posting After Closure..." << std::flush;
    EventQueue q;

    TEST_ASSERT(!q.isClosed());
    TEST_ASSERT(q.post(Event::makeCustom(1)));

    q.close();
    TEST_ASSERT(q.isClosed());

    // Subsequent post must be rejected
    bool accepted = q.post(Event::makeCustom(2));
    TEST_ASSERT(!accepted);

    std::vector<Event> batch;
    q.drain(batch);
    TEST_ASSERT(batch.size() == 1);
    TEST_ASSERT(std::get<CustomPayload>(batch[0].payload).code == 1);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 9: Closure While Consumer is Waiting
// ---------------------------------------------------------------------------
static void test_closure_while_consumer_waiting()
{
    std::cout << "[Test 9] Closure While Consumer Waiting..." << std::flush;
    EventQueue q;
    std::atomic<bool> consumerWoke{false};

    std::thread consumer([&q, &consumerWoke]() {
        struct pollfd pfd = { q.wakeupFd(), POLLIN, 0 };
        int ret = ::poll(&pfd, 1, 2000); // Wait up to 2 seconds
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (q.isClosed()) {
                consumerWoke = true;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.close();

    consumer.join();
    TEST_ASSERT(consumerWoke.load() == true);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 10: Producer Activity During Consumer Draining
// ---------------------------------------------------------------------------
static void test_producer_activity_during_drain()
{
    std::cout << "[Test 10] Producer Activity During Draining..." << std::flush;
    EventQueue q;
    const int kTotalEvents = 5000;
    std::atomic<bool> producerDone{false};

    std::thread producer([&q, &producerDone]() {
        for (int i = 0; i < kTotalEvents; ++i) {
            q.post(Event::makeCustom(i));
        }
        producerDone = true;
    });

    std::vector<Event> received;
    while (!producerDone || q.size() > 0) {
        struct pollfd pfd = { q.wakeupFd(), POLLIN, 0 };
        int ret = ::poll(&pfd, 1, 50);
        if (ret > 0) {
            q.pump([&](const Event &ev) {
                received.push_back(ev);
            });
        }
    }

    producer.join();

    // Final drain to ensure zero lost events
    q.pump([&](const Event &ev) {
        received.push_back(ev);
    });

    TEST_ASSERT(received.size() == kTotalEvents);
    for (int i = 0; i < kTotalEvents; ++i) {
        TEST_ASSERT(std::get<CustomPayload>(received[i].payload).code == i);
    }

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 11: Descriptor Cleanup and Lifetime
// ---------------------------------------------------------------------------
static void test_descriptor_cleanup_and_lifetime()
{
    std::cout << "[Test 11] Descriptor Cleanup and Lifetime..." << std::flush;
    int savedFd = -1;
    {
        EventQueue q;
        savedFd = q.wakeupFd();
        TEST_ASSERT(savedFd >= 0);

        // Verify fd is currently valid
        int flags = ::fcntl(savedFd, F_GETFL);
        TEST_ASSERT(flags >= 0);
    }

    // After destruction, savedFd must be closed
    int ret = ::fcntl(savedFd, F_GETFL);
    TEST_ASSERT(ret < 0 && errno == EBADF);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 12: Re-entrancy Protection and Exception Safety
// ---------------------------------------------------------------------------
static void test_reentrancy_and_exception_safety()
{
    std::cout << "[Test 12] Re-entrancy and Exception Safety..." << std::flush;
    EventQueue q;

    q.post(Event::makeCustom(1));
    q.post(Event::makeCustom(2));
    q.post(Event::makeCustom(3));

    std::vector<int> dispatchOrder;

    // Test re-entrancy prevention on pump()
    q.pump([&](const Event &ev) {
        int code = std::get<CustomPayload>(ev.payload).code;
        dispatchOrder.push_back(code);
        if (code == 1) {
            q.post(Event::makeCustom(4));
            size_t innerPumped = q.pump([&](const Event &) {
                TEST_ASSERT(false && "Re-entrant pump must not execute");
            });
            TEST_ASSERT(innerPumped == 0);
        }
    });

    // Outer pump should have processed 1, 2, 3 in strict FIFO order
    TEST_ASSERT(dispatchOrder.size() == 3);
    TEST_ASSERT(dispatchOrder[0] == 1);
    TEST_ASSERT(dispatchOrder[1] == 2);
    TEST_ASSERT(dispatchOrder[2] == 3);

    // Next pump must process Event 4
    dispatchOrder.clear();
    q.pump([&](const Event &ev) {
        dispatchOrder.push_back(std::get<CustomPayload>(ev.payload).code);
    });
    TEST_ASSERT(dispatchOrder.size() == 1);
    TEST_ASSERT(dispatchOrder[0] == 4);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 13: Re-entrant drain() during pump()
// ---------------------------------------------------------------------------
static void test_reentrant_drain_during_pump()
{
    std::cout << "[Test 13] Re-entrant drain() during pump()..." << std::flush;
    EventQueue q;

    q.post(Event::makeCustom(10));
    q.post(Event::makeCustom(20));
    q.post(Event::makeCustom(30));

    std::vector<int> dispatchOrder;
    bool drainAttempted = false;
    size_t drainedCount = 999;

    q.pump([&](const Event &ev) {
        int code = std::get<CustomPayload>(ev.payload).code;
        dispatchOrder.push_back(code);
        if (code == 10) {
            std::vector<Event> innerBatch;
            drainedCount = q.drain(innerBatch);
            drainAttempted = true;
            TEST_ASSERT(drainedCount == 0);
            TEST_ASSERT(innerBatch.empty());
        }
    });

    TEST_ASSERT(drainAttempted);
    TEST_ASSERT(drainedCount == 0);
    // All 3 events must have been dispatched in order, none stolen by drain()
    TEST_ASSERT(dispatchOrder.size() == 3);
    TEST_ASSERT(dispatchOrder[0] == 10);
    TEST_ASSERT(dispatchOrder[1] == 20);
    TEST_ASSERT(dispatchOrder[2] == 30);
    TEST_ASSERT(q.size() == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 14: Concurrent size() queries during active dispatch
// ---------------------------------------------------------------------------
static void test_concurrent_size_during_dispatch()
{
    std::cout << "[Test 14] Concurrent size() during dispatch..." << std::flush;
    EventQueue q;
    const int kEvents = 2000;
    std::atomic<bool> pumpingDone{false};

    for (int i = 0; i < kEvents; ++i) {
        q.post(Event::makeCustom(i));
    }

    // Reader thread polling size() continuously under lock
    std::thread sizeReader([&q, &pumpingDone]() {
        while (!pumpingDone.load(std::memory_order_relaxed)) {
            size_t sz = q.size();
            (void)sz;
        }
    });

    size_t dispatched = q.pump([&](const Event &) {
        // Simulates slight processing
    });

    pumpingDone.store(true, std::memory_order_relaxed);
    sizeReader.join();

    TEST_ASSERT(dispatched == kEvents);
    TEST_ASSERT(q.size() == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 15: Documented Exception-Delivery Behavior
// ---------------------------------------------------------------------------
static void test_exception_delivery_semantics()
{
    std::cout << "[Test 15] Documented Exception Delivery Semantics..." << std::flush;
    EventQueue q;

    q.post(Event::makeCustom(100)); // Throws
    q.post(Event::makeCustom(200)); // Retained
    q.post(Event::makeCustom(300)); // Retained

    // First pump: event 100 throws and is consumed
    bool caught = false;
    try {
        q.pump([&](const Event &ev) {
            int code = std::get<CustomPayload>(ev.payload).code;
            if (code == 100) {
                throw std::runtime_error("Consumer failure on 100");
            }
        });
    } catch (const std::runtime_error &) {
        caught = true;
    }
    TEST_ASSERT(caught);

    // Remaining undispatched events (200, 300) must still be queued and delivered on next pump
    std::vector<int> remaining;
    size_t count = q.pump([&](const Event &ev) {
        remaining.push_back(std::get<CustomPayload>(ev.payload).code);
    });

    TEST_ASSERT(count == 2);
    TEST_ASSERT(remaining.size() == 2);
    TEST_ASSERT(remaining[0] == 200);
    TEST_ASSERT(remaining[1] == 300);
    TEST_ASSERT(q.size() == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 16: Observable Pipe Initialization State
// ---------------------------------------------------------------------------
static void test_pipe_initialization_state()
{
    std::cout << "[Test 16] Pipe Initialization State..." << std::flush;
    EventQueue q;

    TEST_ASSERT(q.isValid() == true);
    TEST_ASSERT(q.wakeupFd() >= 0);

    // After close, wakeupFd remains valid until destruction
    q.close();
    TEST_ASSERT(q.isClosed() == true);
    TEST_ASSERT(q.isValid() == true);
    TEST_ASSERT(q.wakeupFd() >= 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 17: Concurrent Producers with Interleaved Consumer Pumping and Draining
// ---------------------------------------------------------------------------
static void test_producers_with_interleaved_pump_and_drain()
{
    std::cout << "[Test 17] Interleaved Consumer Pump and Drain..." << std::flush;
    EventQueue q;
    const int kNumProducers = 4;
    const int kEventsPerProducer = 1000;
    std::atomic<int> producersFinished{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&q, &producersFinished, p]() {
            for (int i = 0; i < kEventsPerProducer; ++i) {
                int64_t code = (static_cast<int64_t>(p) << 32) | static_cast<int64_t>(i);
                q.post(Event::makeCustom(code));
            }
            producersFinished.fetch_add(1);
        });
    }

    std::vector<Event> collected;
    int iteration = 0;
    while (producersFinished.load() < kNumProducers || q.size() > 0) {
        if ((iteration % 2) == 0) {
            q.pump([&](const Event &ev) {
                collected.push_back(ev);
            });
        } else {
            q.drain(collected);
        }
        iteration++;
        std::this_thread::yield();
    }

    for (auto &t : producers) {
        t.join();
    }

    // Final drain
    q.drain(collected);

    TEST_ASSERT(collected.size() == static_cast<size_t>(kNumProducers * kEventsPerProducer));

    // Verify all sequence IDs accounted for
    std::map<int, int> nextSeq;
    for (int p = 0; p < kNumProducers; ++p) nextSeq[p] = 0;

    for (const auto &ev : collected) {
        int64_t code = std::get<CustomPayload>(ev.payload).code;
        int p = static_cast<int>(code >> 32);
        int seq = static_cast<int>(code & 0xFFFFFFFF);
        TEST_ASSERT(seq == nextSeq[p]);
        nextSeq[p]++;
    }

    for (int p = 0; p < kNumProducers; ++p) {
        TEST_ASSERT(nextSeq[p] == kEventsPerProducer);
    }

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 18: Thread-Safe Warning Threshold
// ---------------------------------------------------------------------------
static void test_warning_threshold_synchronization()
{
    std::cout << "[Test 18] Thread-Safe Warning Threshold..." << std::flush;
    EventQueue q;
    std::atomic<bool> done{false};

    std::thread mutator([&q, &done]() {
        size_t t = 100;
        while (!done.load(std::memory_order_relaxed)) {
            q.setWarningThreshold(t);
            t = (t == 100) ? 200 : 100;
        }
    });

    for (int i = 0; i < 500; ++i) {
        q.post(Event::makeCustom(i));
    }

    done.store(true, std::memory_order_relaxed);
    mutator.join();

    TEST_ASSERT(q.size() == 500);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 19: Exception Recovery and Wakeup Re-signaling
// ---------------------------------------------------------------------------
static void test_exception_wakeup_resignaling()
{
    std::cout << "[Test 19] Exception Recovery and Wakeup Re-signaling..." << std::flush;
    EventQueue q;
    TEST_ASSERT(q.isValid());

    // 1. Post multiple events
    q.post(Event::makeCustom(1, "one"));
    q.post(Event::makeCustom(2, "two"));
    q.post(Event::makeCustom(3, "three"));

    // 2. Dispatcher throws on event 1
    bool exceptionCaught = false;
    try {
        q.pump([&](const Event &ev) {
            int64_t code = std::get<CustomPayload>(ev.payload).code;
            if (code == 1) {
                throw std::runtime_error("Simulated consumer exception on event 1");
            }
        });
    } catch (const std::runtime_error &) {
        // 3. Catch exception
        exceptionCaught = true;
    }
    TEST_ASSERT(exceptionCaught);

    // 4. Asserts that queue still reports pending events (size() == 2)
    TEST_ASSERT(q.size() == 2);

    // 5. Asserts that poll(wakeupFd(), ...) immediately reports readable (POLLIN)
    struct pollfd pfd;
    pfd.fd = q.wakeupFd();
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pollRet = ::poll(&pfd, 1, 0); // Non-blocking check
    TEST_ASSERT(pollRet == 1);
    TEST_ASSERT(pfd.revents & POLLIN);

    // 6. Pumps again with non-throwing dispatcher
    std::vector<int64_t> remaining;
    size_t pumped = q.pump([&](const Event &ev) {
        remaining.push_back(std::get<CustomPayload>(ev.payload).code);
    });

    // 7. Asserts remaining events delivered in order
    TEST_ASSERT(pumped == 2);
    TEST_ASSERT(remaining.size() == 2);
    TEST_ASSERT(remaining[0] == 2);
    TEST_ASSERT(remaining[1] == 3);
    TEST_ASSERT(q.size() == 0);

    // 8. Asserts descriptor is non-readable after final pump
    pfd.revents = 0;
    pollRet = ::poll(&pfd, 1, 0);
    TEST_ASSERT(pollRet == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 20: Consumer Access Synchronization (Concurrent Consumer Rejection)
// ---------------------------------------------------------------------------
static void test_concurrent_consumer_rejection()
{
    std::cout << "[Test 20] Concurrent Consumer Synchronization..." << std::flush;
    EventQueue q;
    TEST_ASSERT(q.isValid());

    q.post(Event::makeCustom(10));
    q.post(Event::makeCustom(20));

    std::atomic<bool> insideDispatcher{false};
    std::atomic<bool> workerCanProceed{false};
    std::atomic<size_t> concurrentPumpResult{999};
    std::atomic<size_t> concurrentDrainResult{999};
    std::atomic<bool> workerFinished{false};

    // Thread running the concurrent consumer attempts
    std::thread worker([&]() {
        // Wait until main thread's pump is inside the dispatcher
        while (!insideDispatcher.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Attempt concurrent pump() while main thread is actively pumping
        size_t pr = q.pump([&](const Event &) {
            TEST_ASSERT(false && "Concurrent pump dispatcher must never be called");
        });
        concurrentPumpResult.store(pr, std::memory_order_release);

        // Attempt concurrent drain() while main thread is actively pumping
        std::vector<Event> batch;
        size_t dr = q.drain(batch);
        concurrentDrainResult.store(dr, std::memory_order_release);
        TEST_ASSERT(batch.empty());

        workerFinished.store(true, std::memory_order_release);
        workerCanProceed.store(true, std::memory_order_release);
    });

    std::vector<int64_t> received;
    size_t count = q.pump([&](const Event &ev) {
        int64_t code = std::get<CustomPayload>(ev.payload).code;
        received.push_back(code);
        if (code == 10) {
            insideDispatcher.store(true, std::memory_order_release);
            // Wait for worker to attempt concurrent pump() and drain()
            while (!workerCanProceed.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    });

    worker.join();

    // Verify concurrent operations failed cleanly by returning 0
    TEST_ASSERT(concurrentPumpResult.load() == 0);
    TEST_ASSERT(concurrentDrainResult.load() == 0);

    // Verify main consumer was unaffected and received all events in order
    TEST_ASSERT(count == 2);
    TEST_ASSERT(received.size() == 2);
    TEST_ASSERT(received[0] == 10);
    TEST_ASSERT(received[1] == 20);
    TEST_ASSERT(q.size() == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 21: Position Coalescing Across Dispatch Boundary
// ---------------------------------------------------------------------------
static void test_position_coalescing_across_dispatch_boundary()
{
    std::cout << "[Test 21] Position Coalescing Across Dispatch Boundary..." << std::flush;
    EventQueue q;
    TEST_ASSERT(q.isValid());

    // 1. Post Position(100) and Custom(1) into m_queue
    q.post(Event::makePosition(100));
    q.post(Event::makeCustom(1));
    TEST_ASSERT(q.size() == 2);

    std::vector<int64_t> firstPumpPositions;
    std::vector<int64_t> firstPumpCustoms;

    // 2. Call pump(): Position(100) and Custom(1) move to m_dispatchQueue
    size_t firstPumpCount = q.pump([&](const Event &ev) {
        if (ev.type == EventType::PositionChanged) {
            firstPumpPositions.push_back(std::get<PositionChangedPayload>(ev.payload).positionMs);
        } else if (ev.type == EventType::Custom) {
            firstPumpCustoms.push_back(std::get<CustomPayload>(ev.payload).code);

            // While m_dispatchQueue is being processed, producer posts new positions into m_queue.
            // Even though m_dispatchQueue has Position(100) already committed,
            // post(Position(200)) goes into m_queue (does not mutate m_dispatchQueue).
            // Then post(Position(300)) arrives in m_queue, and DOES coalesce with Position(200) in m_queue!
            q.post(Event::makePosition(200));
            q.post(Event::makePosition(300));
        }
    });

    // Verify first pump batch is completely immutable and delivered exactly as committed
    TEST_ASSERT(firstPumpCount == 2);
    TEST_ASSERT(firstPumpPositions.size() == 1);
    TEST_ASSERT(firstPumpPositions[0] == 100);
    TEST_ASSERT(firstPumpCustoms.size() == 1);
    TEST_ASSERT(firstPumpCustoms[0] == 1);

    // Verify m_queue now contains the coalesced Position(300)
    TEST_ASSERT(q.size() == 1);

    // 3. Second pump: should receive the coalesced Position(300)
    std::vector<int64_t> secondPumpPositions;
    size_t secondPumpCount = q.pump([&](const Event &ev) {
        if (ev.type == EventType::PositionChanged) {
            secondPumpPositions.push_back(std::get<PositionChangedPayload>(ev.payload).positionMs);
        }
    });

    TEST_ASSERT(secondPumpCount == 1);
    TEST_ASSERT(secondPumpPositions.size() == 1);
    TEST_ASSERT(secondPumpPositions[0] == 300);
    TEST_ASSERT(q.size() == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Test 22: Spurious Wakeup and Empty Queue Tolerance
// ---------------------------------------------------------------------------
static void test_spurious_wakeup_and_empty_queue_tolerance()
{
    std::cout << "[Test 22] Spurious Wakeup and Empty Queue Tolerance..." << std::flush;
    EventQueue q;
    TEST_ASSERT(q.isValid());

    // Scenario A: close() signals the descriptor on an empty queue
    q.close();
    TEST_ASSERT(q.size() == 0);

    struct pollfd pfd;
    pfd.fd = q.wakeupFd();
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pollRet = ::poll(&pfd, 1, 0);
    TEST_ASSERT(pollRet == 1);
    TEST_ASSERT(pfd.revents & POLLIN);

    // Consumer wakes up and calls pump(); must return 0 safely without calling dispatcher
    bool dispatcherInvoked = false;
    size_t count = q.pump([&](const Event &) {
        dispatcherInvoked = true;
    });
    TEST_ASSERT(count == 0);
    TEST_ASSERT(!dispatcherInvoked);

    // After pump, wakeup pipe is drained
    pfd.revents = 0;
    pollRet = ::poll(&pfd, 1, 0);
    TEST_ASSERT(pollRet == 0);

    // Scenario B: Signal remains in pipe after batch already consumed
    EventQueue q2;
    TEST_ASSERT(q2.isValid());

    q2.post(Event::makeCustom(1));

    // Consumer pumps; during dispatcher for Event 1, close() is called to inject a spurious wakeup byte
    size_t firstPumpCount = q2.pump([&](const Event &ev) {
        TEST_ASSERT(std::get<CustomPayload>(ev.payload).code == 1);
        q2.close(); // Injects wakeup byte into pipe after initial drainPipe() completed
    });
    TEST_ASSERT(firstPumpCount == 1);
    TEST_ASSERT(q2.size() == 0);

    // Wakeup pipe is still readable due to close() signal even though queue is empty
    pfd.fd = q2.wakeupFd();
    pfd.revents = 0;
    pollRet = ::poll(&pfd, 1, 0);
    TEST_ASSERT(pollRet == 1);
    TEST_ASSERT(pfd.revents & POLLIN);

    // Consumer wakes up on POLLIN, calls pump(); must return 0 safely and drain descriptor
    dispatcherInvoked = false;
    size_t secondPumpCount = q2.pump([&](const Event &) {
        dispatcherInvoked = true;
    });
    TEST_ASSERT(secondPumpCount == 0);
    TEST_ASSERT(!dispatcherInvoked);

    pfd.revents = 0;
    pollRet = ::poll(&pfd, 1, 0);
    TEST_ASSERT(pollRet == 0);

    std::cout << " PASSED\n";
}

// ---------------------------------------------------------------------------
// Main test runner
// ---------------------------------------------------------------------------
int main()
{
    std::cout << "=== Natsuyume Core: EventQueue Hardening Test Suite ===\n";

    test_fifo_ordering();
    test_concurrent_producers();
    test_single_consumer_batch_retrieval();
    test_consecutive_position_coalescing();
    test_position_separated_by_lifecycle();
    test_drain_then_swap_wakeup();
    test_empty_queue_pumping();
    test_posting_after_closure();
    test_closure_while_consumer_waiting();
    test_producer_activity_during_drain();
    test_descriptor_cleanup_and_lifetime();
    test_reentrancy_and_exception_safety();

    // Additional hardening tests
    test_reentrant_drain_during_pump();
    test_concurrent_size_during_dispatch();
    test_exception_delivery_semantics();
    test_pipe_initialization_state();
    test_producers_with_interleaved_pump_and_drain();
    test_warning_threshold_synchronization();

    // Final correctness corrections
    test_exception_wakeup_resignaling();
    test_concurrent_consumer_rejection();
    test_position_coalescing_across_dispatch_boundary();
    test_spurious_wakeup_and_empty_queue_tolerance();

    std::cout << "\nAll 22 EventQueue tests passed successfully!\n";
    return 0;
}
