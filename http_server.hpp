#pragma once

#include <map>
#include <memory>
#include <string>
#include "expected.hpp"
#include "io_context.hpp"
#include "stop_source.hpp"
#include "http_codec.hpp"

struct http_server : std::enable_shared_from_this<http_server> {
    using pointer = std::shared_ptr<http_server>;

    static pointer make() {
        return std::make_shared<pointer::element_type>();
    }

    struct http_request {
        std::string url;
        http_method method; // GET, POST, PUT, ...
        std::string body;

        http_response_writer<> *m_res_writer = nullptr;
        callback<> m_resume;

        void write_response(
            int status, std::string_view content,
            std::string_view content_type = "text/plain;charset=utf-8") {
            m_res_writer->begin_header(status);
            m_res_writer->_write_header("Server", "co_http");
            m_res_writer->_write_header("Content-type", content_type);
            m_res_writer->_write_header("Connection", "keep-alive");
            m_res_writer->_write_header("Content-length",
                                       std::to_string(content.size()));
            m_res_writer->_end_header();
            m_res_writer->_write_body(content);
            m_resume();
        }
    };

    struct http_router {
        std::map<std::string, callback<http_request &>> m_routes;

        void route(std::string url, callback<http_request &> cb) {
            // set callback function for url
            m_routes.insert_or_assign(url, std::move(cb));
        }

        void do_handle(http_request &request) {
            // find url matched 
            auto it = m_routes.find(request.url);
            if (it != m_routes.end()) {
                return it->second(multishot_call, request);
            }
            // cannot find url;
            return request.write_response(404, "404 Not Found");
        }
    };

    struct http_connection_handler
        : std::enable_shared_from_this<http_connection_handler> {
        async_file m_conn;
        bytes_buffer m_readbuf{1024};
        http_request_parser<> m_req_parser;
        http_response_writer<> m_res_writer;
        http_router *m_router = nullptr;
        http_request m_request;

        using pointer = std::shared_ptr<http_connection_handler>;

        static pointer make() {
            return std::make_shared<pointer::element_type>();
        }

        void do_start(http_router *router, int connfd) {
            m_router = router;
            m_conn = async_file{connfd};
            return do_read();
        }

        void do_read() {
            // notice: TCP based on stream
            // set a 3s timer
            // if in 3s period haven't recieved any request
            // then client has given up, close the connection 
            stop_source stop_io(std::in_place);
            stop_source stop_timer(std::in_place);
            io_context::get().set_timeout(
                std::chrono::seconds(10),
                [stop_io] {
                    // when timer finished first, stop reading
                    stop_io.request_stop();
                },
                stop_timer);
            // start reading
            return m_conn.async_read(
                m_readbuf,
                [self = shared_from_this(),
                 stop_timer](expected<size_t> ret) {
                    // when finished reading, stop the timer
                    stop_timer.request_stop();
                    if (ret.error()) {
                        // if write error, then give up connection
                        return;
                    }
                    size_t n = ret.value();
                    // n == 0 -> EOF, client close the connection
                    if (n == 0) {
                        return;
                    }
                    // read successfully, push it into parsing
                    self->m_req_parser.push_chunk(
                        self->m_readbuf.subspan(0, n));
                    if (!self->m_req_parser.request_finished()) {
                        return self->do_read();
                    } else {
                        return self->do_handle();
                    }
                },
                stop_io);
        }

        void do_handle() {
            m_request.url = m_req_parser.url();
            m_request.method = m_req_parser.method();
            m_request.body = std::move(m_req_parser.body());
            m_request.m_res_writer = &m_res_writer;
            m_request.m_resume = [self = shared_from_this()] {
                self->do_write(self->m_res_writer.buffer());
            };
            m_req_parser.reset_state();
            m_router->do_handle(m_request);
        }

        void do_write(bytes_const_view buffer) {
            return m_conn.async_write(buffer, [self = shared_from_this(),
                                               buffer](expected<size_t> ret) {
                if (ret.error()) {
                    // if write error, then give up connection
                    return;
                }
                auto n = ret.value();

                if (buffer.size() == n) {
                    self->m_res_writer.reset_state();
                    return self->do_read();
                }
                return self->do_write(buffer.subspan(n));
            });
        }
    };

    async_file m_listening;
    address_resolver::address m_addr;
    http_router m_router;

    http_router &get_router() {
        return m_router;
    }

    void do_start(std::string name, std::string port) {
        address_resolver resolver;
        auto entry = resolver.resolve(name, port);
        m_listening = async_file::async_bind(entry);
        return do_accept();
    }

    void do_accept() {
        return m_listening.async_accept(m_addr, [self = shared_from_this()](
                                                    expected<int> ret) {
            auto connfd = ret.expect("accept");
            // std::cerr << "accept a connection from id: " << connfd << '\n';
            http_connection_handler::make()->do_start(&self->m_router, connfd);
            return self->do_accept();
        });
    }
};