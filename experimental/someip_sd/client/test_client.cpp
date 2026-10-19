/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

// Test application: requests a service and subscribes an eventgroup via the SD daemon, and receives the
// notifications on a plain UDP socket (Linux stand-in for the io-sock event module).

#include "experimental/someip_sd/client/sd_client.h"

#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{

struct Options
{
    std::uint16_t service{0x1234U};
    std::uint16_t instance{0x5678U};
    std::uint16_t eventgroup{0x4465U};
    std::uint8_t major{0x00U};
    std::uint32_t minor{0xFFFFFFFFU};
    std::uint16_t event_port{30600U};
};

std::mutex output_mutex;

void Print(const std::string& text)
{
    std::lock_guard<std::mutex> lock{output_mutex};
    std::cout << "MW:COM:SomeIPBinding: " <<text << std::endl;
}

std::string Hex(std::uint32_t value, int width)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(width) << std::setfill('0') << value;
    return stream.str();
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string name{argv[index]};
        const auto value = static_cast<std::uint32_t>(std::stoul(argv[index + 1], nullptr, 0));
        if (name == "--service")
        {
            options.service = static_cast<std::uint16_t>(value);
        }
        else if (name == "--instance")
        {
            options.instance = static_cast<std::uint16_t>(value);
        }
        else if (name == "--eventgroup")
        {
            options.eventgroup = static_cast<std::uint16_t>(value);
        }
        else if (name == "--major")
        {
            options.major = static_cast<std::uint8_t>(value);
        }
        else if (name == "--minor")
        {
            options.minor = value;
        }
        else if (name == "--event-port")
        {
            options.event_port = static_cast<std::uint16_t>(value);
        }
        else
        {
            return false;
        }
    }
    return (argc % 2) == 1;
}

std::uint32_t ReadBigEndian32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void ReceiveEvents(std::uint16_t port, const std::atomic<bool>& running)
{
    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        Print("event receiver: socket() failed");
        return;
    }
    const int enable{1};
    static_cast<void>(::setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        Print("event receiver: bind() to port " + std::to_string(port) + " failed");
        ::close(socket_fd);
        return;
    }
    Print("event receiver: listening on UDP port " + std::to_string(port));

    std::array<std::uint8_t, 2048U> buffer{};
    constexpr std::size_t kHeaderSize{16U};
    while (running.load())
    {
        pollfd poll_fd{socket_fd, POLLIN, 0};
        if (::poll(&poll_fd, 1, 200) <= 0)
        {
            continue;
        }
        const auto received = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (received < static_cast<ssize_t>(kHeaderSize))
        {
            continue;
        }
        const auto message_id = ReadBigEndian32(&buffer[0]);
        const auto length = ReadBigEndian32(&buffer[4]);
        const auto request_id = ReadBigEndian32(&buffer[8]);
        const auto payload_size = static_cast<std::size_t>(received) - kHeaderSize;

        std::ostringstream text;
        text << "event: service " << Hex(message_id >> 16U, 4) << " event " << Hex(message_id & 0xFFFFU, 4)
             << " session " << Hex(request_id & 0xFFFFU, 4) << " type " << Hex(buffer[14], 2) << " length " << length
             << " payload";
        for (std::size_t index = 0U; (index < payload_size) && (index < 16U); ++index)
        {
            text << " " << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<std::uint32_t>(buffer[kHeaderSize + index]);
        }
        Print(text.str());
    }
    ::close(socket_fd);
}

}  // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: " << argv[0]
                  << " [--service 0x1234] [--instance 0x5678] [--eventgroup 0x4465] [--major 0]"
                     " [--minor 0xffffffff] [--event-port 30600]"
                  << std::endl;
        return EXIT_FAILURE;
    }

    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    std::atomic<bool> running{true};
    std::thread receiver{[&options, &running]() {
        ReceiveEvents(options.event_port, running);
    }};

    score::someip_sd::SdClient client{
        [](std::uint16_t service, std::uint16_t instance, bool available) {
            Print("availability: " + Hex(service, 4) + "." + Hex(instance, 4) +
                  (available ? " available" : " not available"));
        },
        [](std::uint16_t service, std::uint16_t instance, std::uint16_t eventgroup, bool acknowledged) {
            Print("subscription: " + Hex(service, 4) + "." + Hex(instance, 4) + " eventgroup " + Hex(eventgroup, 4) +
                  (acknowledged ? " acknowledged" : " not acknowledged"));
        }};

    int exit_code{EXIT_SUCCESS};
    const auto client_id = client.Connect(std::chrono::seconds{5});
    if (!client_id.has_value())
    {
        Print("connecting to the SD daemon failed (is someip_sd_daemon running in the same network namespace?)");
        exit_code = EXIT_FAILURE;
    }
    else
    {
        Print("registered at SD daemon as client " + Hex(client_id.value(), 4));
        client.RequestService(options.service, options.instance, options.major, options.minor);
        client.Subscribe(options.service, options.instance, options.eventgroup, options.major);

        int signal_number{0};
        sigwait(&signals, &signal_number);

        client.Unsubscribe(options.service, options.instance, options.eventgroup);
        client.ReleaseService(options.service, options.instance);
    }

    running.store(false);
    receiver.join();
    return exit_code;
}
