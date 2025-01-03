#pragma once

#include <map>
#include <chrono>
#include "callback.hpp"
#include "stop_source.hpp"

struct timer_context {
    struct _timer_entry {
        callback<> m_call;
        stop_source m_stop;
    };

    std::multimap<std::chrono::steady_clock::time_point, _timer_entry>
        m_timer_heap;

    timer_context() = default;
    timer_context(timer_context &&) = delete;

    void set_timeout(std::chrono::steady_clock::duration dt, callback<> call,
        stop_source stop = {}) {
        auto expire_time = std::chrono::steady_clock::now() + dt;
        auto it = m_timer_heap.insert(
            {expire_time, _timer_entry{std::move(call), stop}});
        stop.set_stop_callback([this, it] {
            auto call = std::move(it->second.m_call);
            m_timer_heap.erase(it);
            call();
        });
    }

    std::chrono::steady_clock::duration duration_to_next_timer() {
        for (auto it = m_timer_heap.begin(); it != m_timer_heap.end(); it = m_timer_heap.erase(it)) {
            auto now = std::chrono::steady_clock::now();
            if (it->first <= now) {
                // if timer was expired, callback and erase
                it->second.m_stop.clear_stop_callback();
                auto call = std::move(it->second.m_call);
                call();
            } else {
                return it->first - now;
            }
        }
        return std::chrono::nanoseconds(-1);
    }

    bool is_empty() const {
        return m_timer_heap.empty();
    }
};