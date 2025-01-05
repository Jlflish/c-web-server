#include "io_context.hpp"
#include "http_server.hpp"
#include "file_utils.hpp"
#include <iostream>
#include <vector>

struct Message {
    std::string user;
    std::string content;
};

std::string msg_list;

void server() {
    io_context ctx;
    auto server = http_server::make();
    server->get_router().route("/", [](http_server::http_request &request) {
        std::string response = file_get_content("index.html");
        request.write_response(200, response, "text/html;charset=utf-8");
    });
    server->get_router().route("https://code.jquery.com/jquery-3.5.1.min.js", [](http_server::http_request &request) {
        std::string response = file_get_content("https://code.jquery.com/jquery-3.5.1.min.js");
        request.write_response(200, response, "text/javascript");
    });
    server->get_router().route("/send", [](http_server::http_request &request) {
        msg_list += request.body + "\n";
        request.write_response(200, "msg get");
    });
    server->get_router().route("/recv", [](http_server::http_request &request) {
        std::cout << "get a message\n";
        request.write_response(200, msg_list);
    });
    server->do_start("localhost", "8080");
    ctx.join();
}

int main() {
    try {
        server();
    } catch (std::system_error const &e)  {
        // std::cerr << e.what() << '\n';
    }
    return 0;
}
