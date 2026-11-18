#pragma once

#include <cstddef>
#include <deque>
#include <mutex>

/**
 * Samples passed from the emulation to the host's audio output.
 *
 * An IAudioOutput's pullSamples() runs on the host's audio thread while the
 * emulation pushes from its own, so the queue is locked: an unlocked
 * std::deque frees and reallocates its blocks under the other thread and
 * corrupts the heap (#391).
 */
class SampleQueue {
public:
    /** Add a sample. Past `cap` samples, the oldest is dropped (or, with
     *  dropOldest false, the new one is). */
    void push(float s, size_t cap, bool dropOldest = true) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_q.size() >= cap) {
            if (!dropOldest) return;
            m_q.pop_front();
        }
        m_q.push_back(s);
    }

    /** Take up to `max` samples, oldest first. @return how many. */
    int pull(float* buffer, int max) {
        std::lock_guard<std::mutex> lock(m_mutex);
        int n = 0;
        while (n < max && !m_q.empty()) {
            buffer[n++] = m_q.front();
            m_q.pop_front();
        }
        return n;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_q.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_q.clear();
    }

    /** A copy of the queued samples (for tests). */
    std::deque<float> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_q;
    }

private:
    mutable std::mutex m_mutex;
    std::deque<float> m_q;
};
