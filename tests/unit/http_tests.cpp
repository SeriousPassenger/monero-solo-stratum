/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#include "monero_solo/http.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

void require(const bool condition, const char *const message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t unused_tcp_port()
{
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(descriptor >= 0, "could not create port probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(descriptor, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0,
            "port probe bind failed");
    socklen_t length = sizeof(address);
    require(::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0,
            "port probe getsockname failed");
    const auto port = ntohs(address.sin_port);
    (void)::close(descriptor);
    return port;
}

std::string exchange(const std::uint16_t port, const std::string_view request)
{
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(descriptor >= 0, "could not create HTTP client socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::connect(descriptor, reinterpret_cast<sockaddr *>(&address),
                      sizeof(address)) == 0,
            "could not connect to HTTP server");
    std::size_t offset = 0;
    while (offset < request.size()) {
        const ssize_t count = ::send(descriptor, request.data() + offset,
                                     request.size() - offset, MSG_NOSIGNAL);
        require(count > 0, "HTTP request write failed");
        offset += static_cast<std::size_t>(count);
    }
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (count == 0) break;
        require(count > 0, "HTTP response read failed");
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    (void)::close(descriptor);
    return response;
}

Json response_body(const std::string &wire)
{
    const auto split = wire.find("\r\n\r\n");
    require(split != std::string::npos, "HTTP response omitted header terminator");
    return Json::parse(wire.substr(split + 4U));
}

void test_transport_validation()
{
    const std::uint16_t port = unused_tcp_port();
    monero_solo::HttpServerConfig config;
    config.listen = "127.0.0.1:" + std::to_string(port);
    config.worker_threads = 2;
    monero_solo::HttpServer server(
        config, [](const monero_solo::HttpRequest &) {
            return monero_solo::HttpResponse{
                200,
                "{\"schema_version\":1,\"generated_at\":\"2026-01-01T00:00:00.000000Z\","
                "\"data\":{}}",
                {}};
        });
    server.start();

    const std::string zero = exchange(
        port, "GET /v1/health/live HTTP/1.1\r\nHost: localhost\r\n"
              "Content-Length: 00\r\nConnection: close\r\n\r\n");
    require(zero.starts_with("HTTP/1.1 200 OK\r\n"),
            "numeric zero Content-Length was rejected");

    const std::string transfer = exchange(
        port, "GET /v1/health/live HTTP/1.1\r\nHost: localhost\r\n"
              "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
    require(transfer.starts_with("HTTP/1.1 400 Bad Request\r\n"),
            "Transfer-Encoding request was accepted");
    const Json transfer_error = response_body(transfer);
    require(transfer_error["error"]["code"] == "invalid_query" &&
                transfer_error["generated_at"].is_string() &&
                transfer_error["generated_at"].get_ref<const std::string &>().size() == 27U,
            "transport error envelope omitted its timestamp");

    const std::string body = exchange(
        port, "GET /v1/health/live HTTP/1.1\r\nHost: localhost\r\n"
              "Content-Length: 1\r\nConnection: close\r\n\r\nx");
    require(body.starts_with("HTTP/1.1 400 Bad Request\r\n"),
            "nonempty GET body was accepted");

    const std::string duplicate = exchange(
        port, "GET /v1/health/live HTTP/1.1\r\nHost: localhost\r\n"
              "Host: duplicate\r\nConnection: close\r\n\r\n");
    require(duplicate.starts_with("HTTP/1.1 400 Bad Request\r\n"),
            "duplicate HTTP header was accepted");
    server.stop();
}

} // namespace

int main()
{
    try {
        test_transport_validation();
        return 0;
    }
    catch (const std::exception &failure) {
        const ssize_t message_result =
            ::write(STDERR_FILENO, failure.what(),
                    std::char_traits<char>::length(failure.what()));
        const ssize_t newline_result = ::write(STDERR_FILENO, "\n", 1U);
        (void)message_result;
        (void)newline_result;
        return 1;
    }
}
