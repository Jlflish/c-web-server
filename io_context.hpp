#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <system_error>
#include <cassert>
#include <array>
#include "timer_context.hpp"
#include "bytes_buffer.hpp"
#include "expected.hpp"

struct io_context : timer_context {
    int m_epfd;
    size_t m_epcount = 0;

    static inline thread_local io_context *g_instance = nullptr;

    io_context()
        : m_epfd(convert_error(epoll_create1(0)).expect("epoll_create")) {
        g_instance = this;        
    }

    void join() {
        std::array<struct epoll_event, 128> events;
        while (!is_empty()) {
            std::chrono::nanoseconds dt = duration_to_next_timer();
#if HAS_epoll_pwait2
            struct timespec timeout, *timeoutp = nullptr;
            if (dt.count() >= 0) {
                timeout.tv_sec = dt.count() / 1e9;
                timeout.tv_nsec = dt.count() % 1e9;
                timeoutp = &timeout;
            }
            int ret = convert_error(epoll_pwait2(m_epfd, events.data(), events.size(),
                timeoutp, nullptr)).expect("epoll_pwait2");
#else
            int timeout_ms = -1;
            if (dt.count() >= 0) {
                timeout_ms = dt.count() / 1e6;
            }
            int ret = convert_error(epoll_pwait(m_epfd, events.data(), events.size(),
                timeout_ms, nullptr)).expect("epoll_pwait");
#endif
            for (int i = 0; i < ret; i++) {
                auto call = callback<>::from_address(events[i].data.ptr);
                call();
                --m_epcount;
            }
        }
    }

    ~io_context() {
        close(m_epfd);
        g_instance = nullptr;
    }

    [[gnu::const]] static io_context &get() {
        assert(g_instance);
        return *g_instance;
    }

    bool is_empty() const {
        return timer_context::is_empty() && m_epcount == 0;
    }
};

struct file_descriptor {
    int m_fd = -1;

    file_descriptor() = default;

    explicit file_descriptor(int fd) : m_fd(fd) {}
    
    file_descriptor(file_descriptor &&init) noexcept : m_fd(std::move(init.m_fd)) {
        init.m_fd = -1;
    }

    file_descriptor &operator=(file_descriptor &&that) noexcept {
        std::swap(m_fd, that.m_fd);
        return *this;
    }

    ~file_descriptor() {
        if (m_fd != -1) {
            close(m_fd);
        }
    }
};

std::error_category const &gai_category() {
    static struct final : std::error_category {
        char const *name() const noexcept override {
            return "getaddrinfo";
        }

        std::string message(int err) const override {
            return gai_strerror(err);
        }
    } instance;
    return instance;
}


struct address_resolver {
    struct address_ref {
        struct sockaddr *m_addr;
        socklen_t m_addrlen;
    };

    struct address {
        union {
            struct sockaddr m_addr;
            struct sockaddr_storage m_addr_storage;
        };
        socklen_t m_addrlen = sizeof(struct sockaddr_storage);

        operator address_ref() {
            return {&m_addr, m_addrlen};
        }
    };

    struct address_info {
        struct addrinfo *m_curr = nullptr;

        address_ref get_address() const {
            return {m_curr->ai_addr, m_curr->ai_addrlen};
        }

        int create_socket() const {
            return convert_error(socket(m_curr->ai_family, m_curr->ai_socktype, 
                                        m_curr->ai_protocol)).expect("socket");
        }

        [[nodiscard]] bool next_entry() {
            m_curr = m_curr->ai_next;
            if (m_curr == nullptr) {
                return false;
            } 
            return true;
        }
    };
    
    struct addrinfo *head = nullptr;

    address_info resolve(const std::string &name, const std::string &service) {
        int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &head);
        if (err != 0) {
            throw std::system_error(std::error_code(err, gai_category()), name + ":" + service);
        }
        return {head};
    }

    address_resolver() = default;

    address_resolver(address_resolver && rhs) : head(rhs.head) {
        rhs.head = nullptr;
    }

    ~address_resolver() {
        if (head) {
            freeaddrinfo(head);
        }
    }
};

struct async_file : file_descriptor {
    async_file() = default;

    explicit async_file(int fd) : file_descriptor(fd) {
        int flag = convert_error(fcntl(m_fd, F_GETFL)).expect("F_GETFL");
        flag |= O_NONBLOCK;
        convert_error(fcntl(m_fd, F_SETFL, flag)).expect("F_SETFL");

        struct epoll_event event;
        event.events = EPOLLET;
        event.data.ptr = nullptr;
        convert_error(
            epoll_ctl(io_context::get().m_epfd, EPOLL_CTL_ADD, m_fd, &event)
        ).expect("EPOLL_CTL_ADD");
    }

    void _epoll_callback(callback<> &&resume, uint32_t events, stop_source stop) {
        struct epoll_event event;
        event.events = events;
        event.data.ptr = resume.get_address();
        convert_error(
            epoll_ctl(io_context::get().m_epfd, EPOLL_CTL_MOD, m_fd, &event))
            .expect("EPOLL_CTL_MOD");
        ++io_context::get().m_epcount;
        stop.set_stop_callback([resume_ptr = resume.leak_addresss()] {
            callback<>::from_address(resume_ptr)();
        });
    }

    void async_read(bytes_view buf, callback<expected<size_t>> call,
                    stop_source stop = {}) {
#if USE_LEVEL_TRIGGER
        return _epoll_callback(
            [this, buf, call = std::move(call), stop]() mutable {
                if (stop.stop_requested()) {
                    stop.clear_stop_callback();
                    return call(-ECANCELED);
                }
                auto res = convert_error<size_t>(read(m_fd, buf.data(), buf.size()));
                stop.clear_stop_callback();
                return call(res);
            },
            EPOLLIN | EPOLLERR | EPOLLONESHOT, stop);
#else
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return call(-ECANCELED);
        }
        auto res = convert_error<size_t>(read(m_fd, buf.data(), buf.size()));
        if (!res.is_error(EAGAIN)) {
            stop.clear_stop_callback();
            return call(res);
        }

        return _epoll_callback(
            [this, buf, call = std::move(call), stop]() mutable {
                return async_read(buf, std::move(call), stop);
            },
            EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT, stop);
#endif
    }

    void async_write(bytes_const_view buf, callback<expected<size_t>> call,
                     stop_source stop = {}) {
#if USE_LEVEL_TRIGGER
        return _epoll_callback(
            [this, buf, call = std::move(call), stop]() mutable {
                if (stop.stop_requested()) {
                    stop.clear_stop_callback();
                    return call(-ECANCELED);
                }
                auto res = convert_error<size_t>(write(m_fd, buf.data(), buf.size()));
                return call(res);
            },
            EPOLLOUT | EPOLLERR | EPOLLONESHOT, stop);
#else
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return call(-ECANCELED);
        }
        auto res = convert_error<size_t>(write(m_fd, buf.data(), buf.size()));
        if (!res.is_error(EAGAIN)) {
            stop.clear_stop_callback();
            return call(res);
        }

        return _epoll_callback(
            [this, buf, call = std::move(call), stop]() mutable {
                return async_write(buf, std::move(call), stop);
            },
            EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT, stop);
#endif
    }

    void async_accept(address_resolver::address &addr, 
                      callback<expected<int>> call,
                      stop_source stop = {}) {
#if USE_LEVEL_TRIGGER
        return _epoll_callback(
            [this, &addr, call = std::move(call), stop]() mutable {
                if (stop.stop_requested()) {
                    stop.clear_stop_callback();
                    return call(-ECANCELED);
                }
                auto res = convert_error<int>(accept(m_fd, &addr.m_addr, &addr.m_addrlen));
                return call(res);
            },
            EPOLLIN | EPOLLERR | EPOLLONESHOT, stop);
#else
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return call(-ECANCELED);
        }
        auto res = convert_error<int>(accept(m_fd, &addr.m_addr, &addr.m_addrlen));
        if (!res.is_error(EAGAIN)) {
            stop.clear_stop_callback();
            return call(res);
        }

        return _epoll_callback(
            [this, &addr, call = std::move(call), stop]() mutable {
                return async_accept(addr, std::move(call), stop);
            },
            EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT, stop);
#endif
    }

    void async_connect(const address_resolver::address_info &addr, 
                       callback<expected<int>> call,
                       stop_source stop = {}) {
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return call(-ECANCELED);
        }
        auto addr_ptr = addr.get_address();
        auto res = convert_error(connect(m_fd, addr_ptr.m_addr, addr_ptr.m_addrlen));
        if (!res.is_error(EINPROGRESS)) {
            stop.clear_stop_callback();
            return call(res);
        }

        return _epoll_callback(
            [this, call = std::move(call), stop]() mutable {
                if (stop.stop_requested()) {
                    stop.clear_stop_callback();
                    return call(-ECANCELED);
                }
                int res;
                socklen_t res_len = sizeof(res);
                convert_error(
                    getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &res, &res_len)
                ).expect("getsockpot");
                if (res > 0) {
                    res = -res;
                }
                stop.clear_stop_callback();
                return call(res);
            },
            EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT, stop);
    }

    static async_file async_bind(address_resolver::address_info const &addr) {
        auto sock = async_file{addr.create_socket()};
        auto server_addr = addr.get_address();
        int on = 1;
        setsockopt(sock.m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(sock.m_fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        convert_error(bind(sock.m_fd, server_addr.m_addr, server_addr.m_addrlen))
            .expect("bind");
        convert_error(listen(sock.m_fd, SOMAXCONN)).expect("listen");
        return sock;
    }

    async_file(async_file &&) = default;
    async_file &operator = (async_file &&) = default;

    ~async_file() {
        if (m_fd != -1) {
            epoll_ctl(io_context::get().m_epfd, EPOLL_CTL_DEL, m_fd, nullptr);
        }
    }

    explicit operator bool() noexcept {
        return m_fd != -1;
    }
};
