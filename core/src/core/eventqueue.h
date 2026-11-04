#ifndef EVENTQUEUE_H
#define EVENTQUEUE_H

#include <vector>
#include <deque>
#include <mutex>
#include <variant>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <atomic>

namespace Natsuyume {

enum class EventType : uint8_t {
    StateChanged,
    PositionChanged,
    DurationChanged,
    TrackChanged,
    EndOfStream,
    Custom
};

struct StateChangedPayload    { bool isPlaying = false; };
struct PositionChangedPayload { int64_t positionMs = 0; };
struct DurationChangedPayload { int64_t durationMs = 0; };
struct TrackChangedPayload    { int64_t trackId = 0; };
struct EndOfStreamPayload     {};
struct CustomPayload          { int64_t code = 0; std::string text; };

struct Event {
    EventType type = EventType::StateChanged;
    std::variant<
        StateChangedPayload,
        PositionChangedPayload,
        DurationChangedPayload,
        TrackChangedPayload,
        EndOfStreamPayload,
        CustomPayload
    > payload;

    static Event makeState(bool isPlaying) {
        return {EventType::StateChanged, StateChangedPayload{isPlaying}};
    }
    static Event makePosition(int64_t ms) {
        return {EventType::PositionChanged, PositionChangedPayload{ms}};
    }
    static Event makeDuration(int64_t ms) {
        return {EventType::DurationChanged, DurationChangedPayload{ms}};
    }
    static Event makeTrackChanged(int64_t id = 0) {
        return {EventType::TrackChanged, TrackChangedPayload{id}};
    }
    static Event makeEndOfStream() {
        return {EventType::EndOfStream, EndOfStreamPayload{}};
    }
    static Event makeCustom(int64_t code = 0, const std::string &text = "") {
        return {EventType::Custom, CustomPayload{code, text}};
    }
};

class EventQueue {
public:
    EventQueue();
    ~EventQueue();

    // Disable copy and move to ensure clear descriptor ownership
    EventQueue(const EventQueue &) = delete;
    EventQueue &operator=(const EventQueue &) = delete;
    EventQueue(EventQueue &&) = delete;
    EventQueue &operator=(EventQueue &&) = delete;

    // --- Status and Initialization ---
    // Returns true if the non-blocking wakeup pipe was initialized successfully.
    // If false, post() will reject all events, and wakeupFd() returns -1.
    bool isValid() const;

    // --- Producer API (Thread-Safe, Multiple Producers) ---
    // Posts an event to the queue. Returns true if queued, false if queue is closed or invalid.
    //
    // Position Coalescing Contract:
    // Consecutive PositionChanged events are coalesced in-place at the tail of the
    // producer queue (m_queue). Once events have been transferred into the active dispatch
    // batch (m_dispatchQueue), they form a committed consumption batch and are never
    // modified or displaced by subsequent post() calls.
    bool post(const Event &event);

    // --- Consumer API (Strictly Single Consumer) ---
    // Pumps events. Drains wakeup pipe, transfers queued events into the dispatch queue under
    // lock, and invokes dispatcher for each event without holding queue mutex.
    //
    // Consumer Access & Re-entrancy Policy:
    // Only one consumer operation (pump() or drain()) may be active at any given time.
    // Calling pump() or drain() re-entrantly from within a dispatcher callback, or calling
    // them concurrently from another thread, returns 0 immediately to prevent reordering,
    // preemption, or concurrent extraction of the active dispatch batch.
    //
    // Exception Semantics & Wakeup Recovery (At-most-once delivery):
    // The event currently being dispatched is popped from the dispatch queue before
    // invoking dispatcher. If dispatcher throws an exception:
    // 1. The throwing event is consumed and will not be re-dispatched.
    // 2. All subsequent unprocessed events remain queued in the dispatch queue.
    // 3. The consumer lock is released and the wakeup pipe is automatically re-signaled
    //    so any subsequent poll() on wakeupFd() will immediately wake the consumer to
    //    continue processing the remaining events in strict FIFO order.
    size_t pump(const std::function<void(const Event &)> &dispatcher);

    // Direct batch extraction (alternative single-consumer API).
    // Drains wakeup pipe, moves pending events into outBatch under lock, and returns event count.
    // Returns 0 immediately if called re-entrantly or concurrently during an active pump()/drain().
    size_t drain(std::vector<Event> &outBatch);

    // --- Wakeup Notification Contract & Polling ---
    // File descriptor for external event loop polling (poll, epoll, select, ALooper).
    // Returns the read end of the non-blocking wakeup pipe, or -1 only if pipe initialization
    // failed (!isValid()). The descriptor remains valid and open after close() until destruction.
    //
    // Notification Mechanism Contract & Spurious Wakeups:
    // The wakeup descriptor is an edge/level notification trigger rather than an exact
    // representation of queue fullness or event counts:
    // 1. Spurious or redundant wakeups are possible and expected:
    //    - If a producer posts an event immediately after pump() drains the pipe but before
    //      or during batch transfer/dispatch, the wakeup byte will remain in the pipe,
    //      causing a subsequent poll() to wake up even if pump() already consumed that event.
    //    - close() signals the descriptor to unblock a waiting consumer, which may observe an
    //      empty queue if all events were already consumed.
    //    - Consumers MUST tolerate an empty queue (pump() returning 0, drain() returning 0)
    //      after wakeupFd() signals readability.
    // 2. Exception Recovery Invariant:
    //    - If a consumer dispatcher callback throws an exception before all events in the
    //      committed batch are dispatched, onPumpExit() re-signals the pipe if unconsumed
    //      events remain, ensuring external pollers wake to process the remainder.
    // 3. Descriptor Failure Limitation:
    //    - If the wakeup descriptor encounters a fatal I/O failure (e.g. EBADF, EPIPE, EIO),
    //      the error is logged via PMLOG. The EventQueue does not attempt automatic pipe
    //      reconstruction; events remain queued in memory and can still be retrieved via
    //      drain() or pump(), but external descriptor notifications may cease.
    int wakeupFd() const;

    // --- Lifecycle Management ---
    // Closes the queue. Rejects subsequent post() calls and wakes waiting consumer.
    // Descriptors remain open until EventQueue destruction.
    void close();

    // Returns true if close() has been called.
    bool isClosed() const;

    // Current pending event count in queue (thread-safe, safe to call while consumer pumps).
    size_t size() const;

    // Diagnostic threshold for backlog warnings (0 to disable, thread-safe).
    void setWarningThreshold(size_t threshold);

private:
    mutable std::mutex m_mutex;
    std::vector<Event> m_queue;
    int m_wakeupPipe[2] = {-1, -1};
    bool m_valid = false;
    bool m_closed = false;
    size_t m_warningThreshold = 1000;

    // Single-consumer guard & dispatch state
    std::atomic<bool> m_consumerActive{false};
    std::deque<Event> m_dispatchQueue;

    void signalWakeupLocked();
    void drainPipe();
    bool initPipe();
    void cleanupDescriptors();
    void onPumpExit(bool completed);
};

} // namespace Natsuyume

#endif // EVENTQUEUE_H
