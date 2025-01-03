#include <iostream>
#include <vector>
#include <thread>
#include "io_context.hpp"
#include "http_server.hpp"
void server() {
    io_context ctx;
    auto server = http_server::make();
    server->get_router().route("/", [](http_server::http_request &request) {
        std::string response;
        if (request.body.empty()) {
            response = "content empty";
        } else {
            response = "your request is: [" + request.body +
                "], total length: [" + std::to_string(request.body.size()) + "] bytes";
        }
        request.write_response(200, response);
    });
    server->do_start("localhost", "8080");
    ctx.join();
}

int main() {
    std::vector<std::thread> thread_pool;
    const size_t max_threads = std::thread::hardware_concurrency();
    for (size_t i = 0; i < max_threads; ++i) {
        thread_pool.emplace_back(server);
    }
    for (auto &thread : thread_pool) {
        thread.join();
    }
    return 0;
}
