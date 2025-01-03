#pragma once

#include <memory>
#include "callback.hpp"

struct stop_source {
    struct _control_block {
        bool m_stop = false;
        callback<> m_call;
    };

    std::shared_ptr<_control_block> m_control;

    stop_source() = default;

    explicit stop_source(std::in_place_t)
        : m_control(std::make_shared<_control_block>()) {}

    static stop_source make() {
        return stop_source(std::in_place);
    }

    bool stop_requested() const noexcept {
        return m_control && m_control->m_stop;
    }

    bool stop_possible() const noexcept {
        return m_control != nullptr;
    }

    void request_stop() const {
        if (!m_control) {
            return;
        }
        m_control->m_stop = true;
        if (m_control->m_call) {
            m_control->m_call();
            m_control->m_call = nullptr;
        }
    }

    void set_stop_callback(callback<> call) const noexcept {
        if (!m_control) {
            return;
        }
        m_control->m_call = std::move(call);
    }

    void clear_stop_callback() const noexcept {
        if (!m_control) {
            return;
        }
        m_control->m_call = nullptr;
    }
};