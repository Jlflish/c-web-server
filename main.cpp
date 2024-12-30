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
            check_error("write", write(connectfd, result.c_str(), read_len));
            close(connectfd);
        });
    }

    for (auto &i : thread_pool) i.join();

    return 0;
}