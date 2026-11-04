#include "eventqueue.h"
#include "logger.h"

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <iterator>

namespace Natsuyume {

EventQueue::EventQueue()
{
    initPipe();
}

EventQueue::~EventQueue()
{
    close();
    cleanupDescriptors();
}

void EventQueue::cleanupDescriptors()
{
    if (m_wakeupPipe[0] != -1) {
        ::close(m_wakeupPipe[0]);
        m_wakeupPipe[0] = -1;
    }
    if (m_wakeupPipe[1] != -1) {
        ::close(m_wakeupPipe[1]);
        m_wakeupPipe[1] = -1;
    }
}

bool EventQueue::initPipe()
{
#if defined(__linux__) && defined(O_CLOEXEC)
    if (::pipe2(m_wakeupPipe, O_NONBLOCK | O_CLOEXEC) == 0) {
        m_valid = true;
        return true;
    }
#endif

    // Fallback to pipe + fcntl
    if (::pipe(m_wakeupPipe) != 0) {
        m_wakeupPipe[0] = m_wakeupPipe[1] = -1;
        m_valid = false;
        m_closed = true;
        PMLOG("EventQueue: failed to create pipe: %s", strerror(errno));
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(m_wakeupPipe[i], F_GETFL, 0);
        if (flags < 0 || ::fcntl(m_wakeupPipe[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            PMLOG("EventQueue: failed to set O_NONBLOCK on pipe[%d]: %s", i, strerror(errno));
            cleanupDescriptors();
            m_valid = false;
            m_closed = true;
            return false;
        }
        int fdFlags = ::fcntl(m_wakeupPipe[i], F_GETFD, 0);
        if (fdFlags < 0 || ::fcntl(m_wakeupPipe[i], F_SETFD, fdFlags | FD_CLOEXEC) < 0) {
            PMLOG("EventQueue: failed to set FD_CLOEXEC on pipe[%d]: %s", i, strerror(errno));
            cleanupDescriptors();
            m_valid = false;
            m_closed = true;
            return false;
        }
    }

    m_valid = true;
    return true;
}

bool EventQueue::isValid() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_valid;
}

int EventQueue::wakeupFd() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_valid ? m_wakeupPipe[0] : -1;
}

void EventQueue::signalWakeupLocked()
{
    if (m_wakeupPipe[1] == -1 || !m_valid) return;

    char byte = 1;
    while (true) {
        ssize_t ret = ::write(m_wakeupPipe[1], &byte, 1);
        if (ret > 0) {
            break;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Pipe buffer is full; descriptor is already in readable (POLLIN) state
                break;
            }
            // Unexpected write error (e.g. EBADF, EPIPE, EIO)
            PMLOG("EventQueue: unexpected write failure on wakeup pipe: %s", strerror(errno));
            break;
        }
    }
}

void EventQueue::drainPipe()
{
    int rfd = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_valid || m_wakeupPipe[0] == -1) return;
        rfd = m_wakeupPipe[0];
    }

    char buf[128];
    while (true) {
        ssize_t ret = ::read(rfd, buf, sizeof(buf));
        if (ret > 0) {
            continue;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Pipe drained completely
                break;
            }
            // Unexpected read error
            PMLOG("EventQueue: unexpected read failure on wakeup pipe: %s", strerror(errno));
            break;
        }
        if (ret == 0) {
            // EOF reached on pipe
            break;
        }
    }
}

bool EventQueue::post(const Event &event)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed || !m_valid) {
        return false;
    }

    // Coalesce consecutive PositionChanged events in-place at the tail
    if (event.type == EventType::PositionChanged &&
        !m_queue.empty() &&
        m_queue.back().type == EventType::PositionChanged) {
        m_queue.back() = event;
    } else {
        m_queue.push_back(event);
    }

    if (m_warningThreshold > 0 && m_queue.size() % m_warningThreshold == 0) {
        PMLOG("EventQueue backlog warning: pending queue size %zu reached multiple of threshold %zu",
              m_queue.size(), m_warningThreshold);
    }

    signalWakeupLocked();
    return true;
}

void EventQueue::onPumpExit(bool completed)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consumerActive.store(false, std::memory_order_release);
    if (!completed && (!m_dispatchQueue.empty() || !m_queue.empty())) {
        signalWakeupLocked();
    }
}

size_t EventQueue::pump(const std::function<void(const Event &)> &dispatcher)
{
    bool expected = false;
    if (!m_consumerActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return 0;
    }

    struct PumpingGuard {
        EventQueue &q;
        bool completed = false;
        ~PumpingGuard() {
            q.onPumpExit(completed);
        }
    } guard{*this};

    // Step 1: Drain wakeup descriptor completely before acquiring queue mutex
    drainPipe();

    // Step 2: Under lock, transfer newly queued events into dispatch queue
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_queue.empty()) {
            if (m_dispatchQueue.empty()) {
                m_dispatchQueue.assign(
                    std::make_move_iterator(m_queue.begin()),
                    std::make_move_iterator(m_queue.end()));
            } else {
                m_dispatchQueue.insert(
                    m_dispatchQueue.end(),
                    std::make_move_iterator(m_queue.begin()),
                    std::make_move_iterator(m_queue.end()));
            }
            m_queue.clear();
        }
    }

    // Step 3: Dispatch events strictly outside queue mutex, popping each event under lock
    // to ensure m_dispatchQueue access is always synchronized with size() and drain().
    size_t count = 0;
    while (true) {
        Event ev;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_dispatchQueue.empty()) {
                break;
            }
            ev = std::move(m_dispatchQueue.front());
            m_dispatchQueue.pop_front();
        }

        count++;
        if (dispatcher) {
            dispatcher(ev);
        }
    }

    guard.completed = true;
    return count;
}

size_t EventQueue::drain(std::vector<Event> &outBatch)
{
    bool expected = false;
    if (!m_consumerActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return 0;
    }

    struct DrainGuard {
        EventQueue &q;
        bool completed = false;
        ~DrainGuard() {
            std::lock_guard<std::mutex> lock(q.m_mutex);
            q.m_consumerActive.store(false, std::memory_order_release);
            if (!completed && (!q.m_dispatchQueue.empty() || !q.m_queue.empty())) {
                q.signalWakeupLocked();
            }
        }
    } guard{*this};

    drainPipe();

    std::lock_guard<std::mutex> lock(m_mutex);
    size_t total = m_dispatchQueue.size() + m_queue.size();
    if (total == 0) {
        guard.completed = true;
        return 0;
    }

    outBatch.reserve(outBatch.size() + total);

    if (!m_dispatchQueue.empty()) {
        outBatch.insert(
            outBatch.end(),
            std::make_move_iterator(m_dispatchQueue.begin()),
            std::make_move_iterator(m_dispatchQueue.end()));
        m_dispatchQueue.clear();
    }

    if (!m_queue.empty()) {
        outBatch.insert(
            outBatch.end(),
            std::make_move_iterator(m_queue.begin()),
            std::make_move_iterator(m_queue.end()));
        m_queue.clear();
    }

    guard.completed = true;
    return total;
}

void EventQueue::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) {
        return;
    }
    m_closed = true;
    signalWakeupLocked();
}

bool EventQueue::isClosed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

size_t EventQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size() + m_dispatchQueue.size();
}

void EventQueue::setWarningThreshold(size_t threshold)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_warningThreshold = threshold;
}

} // namespace Natsuyume
