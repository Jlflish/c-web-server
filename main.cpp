#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <assert.h>
// C include
#include <iostream>
#include <optional>
#include <thread>
#include <vector>
#include <algorithm>
#include <map>
// C++ include

int check_error(const std::string &error_type, int result) {
    if (result == -1) {
        perror(error_type.c_str());
        throw;
    }
    return result;
}

size_t check_error(const std::string &error_type, ssize_t result) {
    if (result == -1) {
        perror(error_type.c_str());
        throw;
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

    void resolve(const std::string &name, const std::string &service) {
        check_error("resolve", getaddrinfo(name.c_str(), service.c_str(), NULL, &head));
    }

    address_resolved_entry get_first_entry() {
        return {head};
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

std::vector<std::thread> thread_pool;
int main() {
    setlocale(LC_ALL, "zh_CN.UTF-8");

    struct address_resolver resolver("localhost", "8080");

    auto address_entry = resolver.get_first_entry();
    
    int listen_sockfd = address_entry.create_socket();
    
    auto server_address = address_entry.get_address();

    check_error("bind", bind(listen_sockfd, server_address.m_addr, server_address.m_addrlen));
    check_error("listen", listen(listen_sockfd, SOMAXCONN));

    while (true) {
        socket_address_storage addr;
        int connectfd = check_error("accept", accept(listen_sockfd, &addr.m_addr, &addr.m_addrlen));
        thread_pool.emplace_back([connectfd] {
            char buff[1024];
            _http_base_parser parser;
            do {
                size_t read_len = check_error("read", read(connectfd, buff, sizeof buff));
                parser.push_chunk(std::string(buff, read_len));
            } while (!parser.header_finished());
            std::cout << "get request header: " << parser.headers_raw() << '\n';
            std::cout << "get request content: " << parser.body() << '\n';
            size_t length = parser.body().size();
            std::string response = std::string("HTTP/1.1 200 OK\r\n") + 
                                   std::string("Server: co_http\r\n") +
                                   std::string("Connection: close\r\n") +
                                   std::string("Content-length: " + std::to_string(length) + "\r\n\r\n") +
                                   parser.body();
            check_error("write", write(connectfd, response.c_str(), response.size()));
            close(connectfd);
        });
    }

    for (auto &i : thread_pool) i.join();
    close(listen_sockfd);
    return 0;
}