#pragma once

#include <string>
#include <map>
#include <algorithm>
#include <cassert>
#include "bytes_buffer.hpp"
#include "enum_parser.hpp"

struct http11_request_parser {
    bytes_buffer m_header;
    std::string m_headline;
    std::string m_body;
    std::map<std::string, std::string> m_header_keys;
    bool m_header_finished{};

    void reset_state() {
        m_header.clear();
        std::string().swap(m_headline);
        std::string().swap(m_body);
        std::map<std::string, std::string>().swap(m_header_keys);
        m_header_finished = {};
    }

    [[nodiscard]] bool header_finished() const {
        return m_header_finished;
    }

    void extract_headers() {
        std::string_view header = m_header;
        auto pos = header.find("\r\n", 0, 2);
        for (m_headline = std::string(header.substr(0, pos)); pos != std::string::npos; ) {
            //! skip \r\n
            pos += 2;
            size_t next_pos = header.find("\r\n", pos);
            size_t line_len = next_pos == std::string::npos ? next_pos : next_pos - pos;
            //! cut the content
            std::string_view line = header.substr(pos, line_len);
            size_t colon = line.find(": ", 0, 2);
            if (colon != std::string::npos) {
                //! key : value
                std::string key = std::string(line.substr(0, colon));
                std::string_view value = line.substr(colon + 2);
                //! convert key to lower_case alphabet
                std::transform(key.begin(), key.end(), key.begin(), [](auto i) {
                    if ('A' <= i && i <= 'Z') i = i ^ 'A' ^ 'a';
                    return i;
                });
                m_header_keys.insert_or_assign(std::move(key), value);
            }
            pos = next_pos;
        }
    }

    void push_chunk(bytes_const_view chunk) {
        assert(!m_header_finished);
        size_t old_size = m_header.size();
        m_header.append(chunk);
        std::string_view header = m_header;
        if (old_size < 4) {
            old_size = 4;
        }
        old_size -= 4;
        size_t m_header_len = header.find("\r\n\r\n", old_size, 4);
        if (m_header_len != std::string::npos) {
            //! header already finished
            m_header_finished = true;
            //! remove \r\n\r\n
            m_body = header.substr(m_header_len + 4);
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

    bytes_buffer &headers_raw() {
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

    void push_chunk(bytes_const_view chunk) {
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
struct http_request_parser : _http_base_parser<HeaderParser> {
    http_method method() {
        return parse_enum<http_method>(this->_headline_first());
    }
    std::string url() {
        return this->_headline_second();
    }
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
    bytes_buffer m_buffer;

    void reset_state() {
        m_buffer.clear();
    }

    bytes_buffer &buffer() {
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

    bytes_buffer &buffer() {
        return m_header_writer.buffer();
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

template <class HeaderWriter = http11_header_writer>
struct http_request_writer : _http_base_writer<HeaderWriter> {
    void begin_header(std::string_view method, std::string_view url) {
        this->_begin_header(method, url, "HTTP/1.1");
    }
};

template <class HeaderWriter = http11_header_writer>
struct http_response_writer : _http_base_writer<HeaderWriter> {
    void begin_header(int status) {
        this->_begin_header("HTTP/1.1", std::to_string(status), "OK");
    }
};