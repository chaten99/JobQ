#include "EventHub.h"
#include "helpers/TestSupport.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{

    using jobq::server::EventHub;
    using jobq_test::check;

    class RecordingConnection final : public drogon::WebSocketConnection
    {
    public:
        void send(const char *data, uint64_t size, drogon::WebSocketMessageType type) override
        {
            send(std::string_view(data, size), type);
        }

        void send(std::string_view message, drogon::WebSocketMessageType type) override
        {
            if (fail)
                throw std::runtime_error("send failed");
            if (type == drogon::WebSocketMessageType::Ping)
            {
                token = message;
            }
            else
            {
                std::istringstream input{std::string(message)};
                Json::Value event;
                std::string errors;
                if (!Json::parseFromStream(Json::CharReaderBuilder{}, input, &event, &errors))
                {
                    throw std::runtime_error("invalid event");
                }
                sequences.push_back(std::stoull(event["sequence"].asString()));
                if (onSend)
                    onSend();
            }
        }

        void sendJson(const Json::Value &, drogon::WebSocketMessageType) override {}
        const trantor::InetAddress &localAddr() const override { return address; }
        const trantor::InetAddress &peerAddr() const override { return address; }
        bool connected() const override { return open; }
        bool disconnected() const override { return !open; }
        void shutdown(drogon::CloseCode, const std::string &) override { open = false; }
        void forceClose() override { open = false; }
        void setPingMessage(const std::string &, const std::chrono::duration<double> &) override {}
        void disablePing() override {}

        trantor::InetAddress address;
        bool open{true};
        bool fail{false};
        std::string token;
        std::vector<std::uint64_t> sequences;
        std::function<void()> onSend;
    };

}

int main()
{
    EventHub hub;
    auto fast = std::make_shared<RecordingConnection>();
    auto slow = std::make_shared<RecordingConnection>();
    auto broken = std::make_shared<RecordingConnection>();
    auto departing = std::make_shared<RecordingConnection>();
    broken->fail = true;
    departing->onSend = [&]
    { hub.remove(departing); departing->forceClose(); };
    for (const auto &socket : {fast, slow, broken, departing})
    {
        check(hub.subscribe(socket), "Registers each live subscription");
    }

    using enum jobq::JobStatus;
    std::thread first([&]
                      {
        for (int i = 0; i < 8; ++i) hub.publish({1, Pending, Running, 1}); });
    std::thread second([&]
                       {
        for (int i = 0; i < 8; ++i) hub.publish({2, Running, Completed, 1}); });
    first.join();
    second.join();
    hub.flush();
    check(fast->sequences.size() == 16 && fast->sequences == slow->sequences &&
              fast->sequences.front() == 1 && fast->sequences.back() == 16 &&
              std::adjacent_find(fast->sequences.begin(), fast->sequences.end(),
                                 [](auto a, auto b)
                                 { return b != a + 1; }) == fast->sequences.end(),
          "Concurrent publications share a contiguous sequence across subscribers");
    check(!broken->open && !departing->open, "Delivery errors and concurrent removal do not affect other participants");

    hub.acknowledge(slow, "incorrect ping token");
    for (int i = 0; i < 17; ++i)
    {
        hub.acknowledge(fast, fast->token);
        for (int j = 0; j < 16; ++j)
            hub.publish({3, Running, Failed, 1});
        hub.flush();
    }
    check(!slow->open && slow->sequences.size() == 16 && fast->open && fast->sequences.size() == 288,
          "A nonresponsive client gets one bounded batch and closes on ring overrun");
    check(departing->sequences.size() <= 16, "Removed client receives no future batches");
    hub.close();
    hub.close();
    hub.publish({4, Pending, Running, 1});
    hub.flush();
    check(!fast->open && !hub.subscribe(fast) && fast->sequences.size() == 288,
          "Shutdown closes active clients and rejects later work");
    return jobq_test::testsFailed() ? 1 : 0;
}
