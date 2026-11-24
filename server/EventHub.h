#pragma once

#include "job/Job.h"
#include <drogon/WebSocketConnection.h>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace jobq::server
{

    class EventHub
    {
    public:
        void publish(JobStatusChange change);
        bool subscribe(const drogon::WebSocketConnectionPtr &connection);
        void remove(const drogon::WebSocketConnectionPtr &connection);
        void acknowledge(const drogon::WebSocketConnectionPtr &connection, const std::string &token);
        void flush();
        void close() noexcept;

    private:
        using Clock = std::chrono::steady_clock;
        struct Subscriber
        {
            std::uint64_t next;
            std::string token;
            Clock::time_point deadline{};
        };

        static constexpr std::size_t capacity = 256;
        std::mutex eventsMutex_;
        std::array<JobStatusChange, capacity> events_{};
        std::uint64_t latest_{0};
        std::atomic<bool> closed_{false};
        std::mutex subscribersMutex_;
        std::unordered_map<drogon::WebSocketConnectionPtr, Subscriber> subscribers_;
    };

}
