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
#ifndef SCORE_EXPERIMENTAL_SOMEIP_SD_EVENT_MODULE_EVENT_MODULE_H
#define SCORE_EXPERIMENTAL_SOMEIP_SD_EVENT_MODULE_EVENT_MODULE_H

#include "experimental/someip_sd/protocol/event_module_protocol.h"

#include "score/message_passing/server_factory.h"
#include "score/mw/com/types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace score::someip_sd
{

/// User-space stand-in for the io-sock event module: receives SOME/IP notifications on UDP and offers them as
/// LoLa events (one GenericSkeleton per SOME/IP service instance). Routing is configured at runtime by the
/// SD daemon via event_module_protocol.
class EventModule final
{
  public:
    EventModule() = default;
    ~EventModule();

    EventModule(const EventModule&) = delete;
    EventModule& operator=(const EventModule&) = delete;
    EventModule(EventModule&&) = delete;
    EventModule& operator=(EventModule&&) = delete;

    bool Start();
    void Stop();

    /// Receive loop; returns once running becomes false.
    void Run(const std::atomic<bool>& running);

  private:
    using ServiceInstance = std::pair<std::uint16_t, std::uint16_t>;

    struct Route
    {
        std::uint16_t local_port{0U};
        std::array<std::uint8_t, 4U> remote_ip{};
        std::uint16_t remote_port{0U};
        bool active{false};
        std::vector<std::string> event_names;  // backing storage of the EventInfo names
        std::unique_ptr<score::mw::com::GenericSkeleton> skeleton;
        std::map<std::uint16_t, score::mw::com::GenericSkeletonEvent*> events;
        std::set<std::uint16_t> subscribed;
    };

    // Handle incoming routing commands from the SD daemon (message-passing).
    void HandleCommand(const event_module_protocol::Message& message);
    void AddRoute(const event_module_protocol::Message& message);
    void RemoveRoute(const ServiceInstance& key);
    bool OpenSocket(std::uint16_t port);
    // handle incoming datagrams on the UDP sockets.
    void OnDatagram(std::uint16_t local_port,
                    const std::array<std::uint8_t, 4U>& sender_ip,
                    std::uint16_t sender_port,
                    const std::uint8_t* data,
                    std::size_t size);

    score::message_passing::ServerFactory server_factory_;
    score::cpp::pmr::unique_ptr<score::message_passing::IServer> server_;

    std::mutex mutex_;
    std::map<ServiceInstance, Route> routes_;
    std::map<std::uint16_t, int> sockets_;  // local port -> fd
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint64_t> forwarded_;
};

}  // namespace score::someip_sd

#endif  // SCORE_EXPERIMENTAL_SOMEIP_SD_EVENT_MODULE_EVENT_MODULE_H
