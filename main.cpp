#include <iostream>
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
    try {
        server();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << '\n';
    }
    return 0;
}