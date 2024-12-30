#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

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

struct http_request_parser {
    std::string m_header;
    std::string m_body;
    bool m_header_finished{};
    bool m_body_finished{};

    [[nodiscard]] bool need_more_chunks() const {
        return !m_header_finished;
    }

    void push_chunk(const std::string_view &chunk) {
        if (!m_header_finished) {
            m_header.append(chunk);
            size_t m_header_len = m_header.find("\r\n");
            if (m_header_len != std::string::npos) {
                //! header already finished
                m_header_finished = true;
                m_body = m_header.substr(m_header_len);
                //! pop out the redundant body part
                m_header.resize(m_header_len);
                //! TODO : parse body out
                //! TODO : assert body has no content inside
                m_body_finished = true;
            }
        } else {
            m_body.append(chunk);
        }
    }
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
            size_t read_len = check_error("read", read(connectfd, buff, sizeof buff));
            std::string result(buff, read_len);
            // echo server
            std::cout << result << '\n';
            auto p = result.find("\r\n\r\n");
            if (p != std::string::npos) {

            }
            std::string response = std::string("HTTP/1.1 200 OK\r\n") + 
                                   std::string("Server: co_http\r\n") +
                                   std::string("Connection: close\r\n") +
                                   std::string("Content-length: 12\r\n\r\n") +
                                   std::string("Hello,World!");
            check_error("write", write(connectfd, response.c_str(), response.size()));
            close(connectfd);
        });
    }

    for (auto &i : thread_pool) i.join();

    return 0;
}