#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <assert.h>
//! C include
#include <iostream>
#include <optional>
#include <thread>
#include <vector>
#include <algorithm>
#include <map>
#include <cstring>
#include <type_traits>
#include <utility>
//! C++ include
#include "expected.hpp"
#include "callback.hpp"
//! lab include

//! use gai category to check server's error
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

struct file_descriptor {
    int m_fd = -1;

    file_descriptor() = default;

    explicit file_descriptor(int fd) : m_fd(fd) {}
    
    file_descriptor(file_descriptor &&init) noexcept : m_fd(std::move(init.m_fd)) {
        init.m_fd = -1;
    }

    ~file_descriptor() {
        if (m_fd != -1) {
            close(m_fd);
        }
    }
};

//! check error function can only check system's error
template <class T>
T check_error(const std::string &error_type, T result) {
    if (result == -1) {
        std::cerr << error_type << ' ' << std::strerror(errno) << '\n';
        throw std::system_error(std::error_code(errno, std::system_category()), error_type);
    }
    return result;
}

struct socket_address_fatptr {
    struct sockaddr *m_addr;
    socklen_t m_addrlen;
};

struct socket_address_storage {
    union {
        struct sockaddr m_addr;
        struct sockaddr_storage m_addr_storage;
    };
    socklen_t m_addrlen = sizeof(struct sockaddr_storage);

    operator socket_address_fatptr() {
        return {&m_addr, m_addrlen};
    }
};

struct address_resolved_entry {

    struct addrinfo *m_curr = nullptr;

    socket_address_fatptr get_address() {
        return {m_curr->ai_addr, m_curr->ai_addrlen};
    }

    int create_socket() {
        return check_error("socket", socket(m_curr->ai_family, m_curr->ai_socktype, m_curr->ai_protocol));
    }

    int create_socket_and_bind() {
        int listen_sockfd = create_socket();
        auto server_address = get_address();
        check_error("bind", bind(listen_sockfd, server_address.m_addr, server_address.m_addrlen));
        check_error("listen", listen(listen_sockfd, SOMAXCONN));
        return listen_sockfd;
    }

    [[nodiscard]] bool next_entry() {
        m_curr = m_curr->ai_next;
        if (m_curr == nullptr) {
            return false;
        } 
        return true;
    }
};

struct address_resolver {
    
    struct addrinfo *head = nullptr;

    address_resolver() = default;

    address_resolver(address_resolver && rhs) {
        head = std::move(rhs.head);
    }

    address_resolver(const std::string &name, const std::string &service) {
        resolve(name, service);
    }

    ~address_resolver() {
        freeaddrinfo(head);
    }

    address_resolved_entry resolve(const std::string &name, const std::string &service) {
        int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &head);
        if (err != 0) {
            throw std::system_error(std::error_code(err, gai_category()), name + ":" + service);
        }
        return {head};
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
            epoll_ctl(get().m_epfd, EPOLL_CTL_ADD, m_fd, &event)
        ).expect("EPOLL_CTL_ADD");

    }
};

struct http11_request_parser {
    std::string m_header;
    std::string m_headline;
    std::string m_body;
    std::map<std::string, std::string> m_header_keys;
    bool m_header_finished{};

    void reset_state() {
        std::string().swap(m_header);
        std::string().swap(m_headline);
        std::string().swap(m_body);
        std::map<std::string, std::string>().swap(m_header_keys);
        m_header_finished = {};
    }

    [[nodiscard]] bool header_finished() const {
        return m_header_finished;
    }

    void extract_headers() {
        auto pos = m_header.find("\r\n");
        for (m_headline = m_header.substr(0, pos); pos != std::string::npos; ) {
            //! skip \r\n
            pos += 2;
            size_t next_pos = m_header.find("\r\n", pos);
            size_t line_len = next_pos == std::string::npos ? next_pos : next_pos - pos;
            //! cut the content
            std::string line = m_header.substr(pos, line_len);
            size_t colon = line.find(": ");
            if (colon != std::string::npos) {
                //! key : value
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 2);
                //! convert key to lower_case alphabet
                std::transform(value.begin(), value.end(), value.begin(), [](auto i) {
                    if ('A' <= i && i <= 'Z') i = i ^ 'A' ^ 'a';
                    return i;
                });
                m_header_keys.insert_or_assign(std::move(key), value);
            }
            pos = next_pos;
        }
    }

    void push_chunk(const std::string_view &chunk) {
        assert(!m_header_finished);
        m_header.append(chunk);
        size_t m_header_len = m_header.find("\r\n\r\n");
        if (m_header_len != std::string::npos) {
            //! header already finished
            m_header_finished = true;
            //! remove \r\n\r\n
            m_body = m_header.substr(m_header_len + 4);
            //! pop out the redundant body part
            m_header.resize(m_header_len);
            //! do headers' job
            extract_headers();
        }
    }

    std::string &headline() {
            return m_headline;
        }

    std::map<std::string, std::string> &headers() {
        return m_header_keys;
    }

    std::string &headers_raw() {
        return m_header;
    }

    std::string &extra_body() {
        return m_body;
    }
};

template <class HeaderParser = http11_request_parser>
struct _http_base_parser {
    HeaderParser m_header_parser;
    size_t m_content_length{};
    size_t body_accumulated_size{};
    bool m_body_finished{};

    void reset_state() {
        m_header_parser.reset_state();
        m_content_length = {};
        body_accumulated_size = {};
        m_body_finished = {};
    }

    [[nodiscard]] bool header_finished() {
        return m_header_parser.header_finished();
    }

    [[nodiscard]] bool request_finished() {
        return m_body_finished;
    }

    std::string &headers_raw() {
        return m_header_parser.headers_raw();
    }

    std::string &headline() {
        return m_header_parser.headline();
    }

    std::map<std::string, std::string> &headers() {
        return m_header_parser.headers();
    }

    std::string _headline_first() {
        const auto &line = headline();
        size_t pos = line.find(' ');
        if (pos == std::string::npos) {
            return "";
        }
        return line.substr(0, pos);
    }

    std::string _headline_second() {
        const auto &line = headline();
        auto pos1 = line.find(' ');
        if (pos1 == std::string::npos) {
            return "";
        }
        auto pos2 = line.find(' ', pos1 + 1);
        if (pos2 == std::string::npos) {
            return "";
        }
        return line.substr(pos1 + 1, pos2 - pos1 - 1);
    }

    std::string _headline_third() {
        const auto &line = headline();
        auto pos = line.find(' ');
        if (pos == std::string::npos) {
            return "";
        }
        pos = line.find(' ', pos + 1);
        if (pos == std::string::npos) {
            return "";
        }
        return line.substr(pos);
    }

    std::string &body() {
        return m_header_parser.extra_body();
    }

    size_t _extract_content_length() {
        auto &headers = m_header_parser.headers();
        auto it = headers.find("content-length");
        if (it == headers.end()) {
            return 0;
        }
        try {
            return std::stoi(it->second);
        } catch(std::logic_error const &) {
            return 0;
        }
    }

    void push_chunk(const std::string &chunk) {
        assert(!m_body_finished);
        if (!m_header_parser.header_finished()) {
            m_header_parser.push_chunk(chunk);
            if (m_header_parser.header_finished()) {
                body_accumulated_size = body().size();
                m_content_length = _extract_content_length();
                if (body_accumulated_size >= m_content_length) {
                    m_body_finished = true;
                }
            }
        } else {
            body().append(chunk);
            body_accumulated_size += chunk.size();
            if (body_accumulated_size >= m_content_length) {
                m_body_finished = true;
            }
        }
    }
};

enum class http_method {
    UNKNOWN = -1,
    GET,
    POST,
    PUT,
    DELETE,
    HEAD,
    OPTIONS,
    PATCH,
    TRACE,
    CONNECT,
};

template <class HeaderParser = http11_request_parser>
struct http_response_parser : _http_base_parser<HeaderParser> {
    int status() {
        auto s = this->_headline_second();
        try {
            return std::stoi(s);
        } catch (std::logic_error const &) {
            return -1;
        }
    }
};

struct http11_header_writer {
    std::string m_buffer;

    void reset_state() {
        std::string().swap(m_buffer);
    }

    std::string &buffer() {
        return m_buffer;
    }

    void begin_header(std::string_view first, std::string_view second, std::string_view third) {
        m_buffer.append(first);
        m_buffer.append(" ");
        m_buffer.append(second);
        m_buffer.append(" ");
        m_buffer.append(third);
    }

    void write_header(std::string_view key, std::string_view value) {
        m_buffer.append("\r\n");
        m_buffer.append(key);
        m_buffer.append(": ");
        m_buffer.append(value);
    }

    void end_header() {
        m_buffer.append("\r\n\r\n");
    }
};

template <class HeadWriter = http11_header_writer>
struct _http_base_writer {
    HeadWriter m_header_writer;

    void reset_state() {
        m_header_writer.reset_state();
    }

    void _begin_header(std::string_view first, std::string_view second, std::string_view third) {
        m_header_writer.begin_header(first, second, third);
    }

    HeadWriter &buffer() {
        return m_header_writer;
    }

    void _write_header(std::string_view key, std::string_view value) {
        m_header_writer.write_header(key, value);
    }

    void _end_header() {
        m_header_writer.end_header();
    }

    void _write_body(std::string_view body) {
        m_header_writer.buffer().append(body);
    }
};

std::vector<std::thread> thread_pool;
void server() {
    struct address_resolver resolver;
    auto address_entry = resolver.resolve("localhost", "8080");
    int listen_sockfd = address_entry.create_socket_and_bind();
    while (true) {
        socket_address_storage addr;
        int connectfd = check_error("accept", accept(listen_sockfd, &addr.m_addr, &addr.m_addrlen));
        thread_pool.emplace_back([connectfd] {
            while (true) {
                std::cout << "connection from id: " << connectfd << '\n';
                char buff[1024];
                _http_base_parser parser;

                while (!parser.header_finished()) {
                    size_t read_len = check_error("read", read(connectfd, buff, sizeof buff));
                    if (read_len == 0) {
                        std::cout << "connection id: " << connectfd << ' ' << "closed the connection\n";
                        goto client_close_exit;
                    }
                    parser.push_chunk(std::string(buff, read_len));
                }
                // std::cout << "connection header: " << parser.headers_raw() << '\n';
                _http_base_writer result_writer;
                result_writer._begin_header("HTTP/1.1", "200", "OK");
                result_writer._write_header("Server", "co_http");
                result_writer._write_header("Content-type", "text/html;charset=utf-8");
                result_writer._write_header("Connection", "keep-alive");
                const std::string response = parser.body().empty() ? "your request content is empty" : parser.body();
                result_writer._write_header("Content-length", std::to_string(response.size()));
                result_writer._end_header();
                result_writer._write_body(response);
                auto response_buffer = result_writer.buffer().buffer();
                check_error("write", write(connectfd, response_buffer.c_str(), response_buffer.size()));
            }
            client_close_exit:
            std::cout << "connection closed\n";
            close(connectfd);
        });
    }
    close(listen_sockfd);
}
int main() {
    setlocale(LC_ALL, "zh_CN.UTF-8");
    try {
        server();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << '\n';
    }
    for (auto &i : thread_pool) i.join();
    return 0;
}