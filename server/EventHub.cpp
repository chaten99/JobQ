#include "EventHub.h"

#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <limits>
#include <vector>

namespace jobq::server {

namespace {

std::string encode(std::uint64_t sequence, JobStatusChange change) {
    Json::Value body;
    body["event"] = "job.status_changed";
    body["sequence"] = std::to_string(sequence);
    auto& data = body["data"];
    data["job_id"] = std::to_string(change.id);
    data["previous_status"] = std::string(toString(change.previousStatus));
    data["status"] = std::string(toString(change.status));
    data["attempts_made"] = Json::UInt64(change.attemptsMade);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, body);
}

void closeConnection(const drogon::WebSocketConnectionPtr& connection) noexcept {
    try {
        connection->forceClose();
    } catch (...) {
    }
}

}

void EventHub::publish(JobStatusChange change) {
    std::lock_guard lock(eventsMutex_);
    if (closed_ || latest_ == std::numeric_limits<std::uint64_t>::max() - 1) return;
    ++latest_;
    events_[latest_ % capacity] = change;
}

bool EventHub::subscribe(const drogon::WebSocketConnectionPtr& connection) {
    std::lock_guard subscribersLock(subscribersMutex_);
    if (closed_) return false;
    std::uint64_t next;
    {
        std::lock_guard eventsLock(eventsMutex_);
        next = latest_ + 1;
    }
    return subscribers_.try_emplace(connection, Subscriber{next, {}, {}}).second;
}

void EventHub::remove(const drogon::WebSocketConnectionPtr& connection) {
    std::lock_guard lock(subscribersMutex_);
    subscribers_.erase(connection);
}

void EventHub::acknowledge(const drogon::WebSocketConnectionPtr& connection, const std::string& token) {
    std::lock_guard lock(subscribersMutex_);
    const auto entry = subscribers_.find(connection);
    if (entry != subscribers_.end() && entry->second.token == token) {
        entry->second.token.clear();
    }
}

void EventHub::flush() {
    struct Batch {
        drogon::WebSocketConnectionPtr connection;
        std::string token;
        std::uint64_t first{};
        std::vector<JobStatusChange> changes;
    };
    std::array<JobStatusChange, capacity> events;
    std::uint64_t latest;
    {
        std::lock_guard lock(eventsMutex_);
        events = events_;
        latest = latest_;
    }
    std::vector<Batch> batches;
    {
        std::lock_guard lock(subscribersMutex_);
        const auto now = Clock::now();
        for (auto& [connection, subscriber] : subscribers_) {
            if ((latest >= subscriber.next && latest - subscriber.next >= capacity) ||
                (!subscriber.token.empty() && now >= subscriber.deadline)) {
                batches.push_back({connection, {}, 0, {}});
                continue;
            }
            if (subscriber.token.empty() && subscriber.next <= latest) {
                Batch batch{connection, drogon::utils::secureRandomString(24), subscriber.next, {}};
                for (auto seq = subscriber.next; seq <= latest && batch.changes.size() < 16; ++seq) {
                    batch.changes.push_back(events[seq % capacity]);
                }
                batches.push_back(std::move(batch));
                subscriber.token = batches.back().token;
                subscriber.next += batches.back().changes.size();
                subscriber.deadline = now + std::chrono::seconds{5};
            }
        }
    }
    for (const auto& batch : batches) {
        if (batch.changes.empty()) {
            remove(batch.connection);
            closeConnection(batch.connection);
            continue;
        }
        try {
            if (!batch.connection->connected()) {
                remove(batch.connection);
                continue;
            }
            auto sequence = batch.first;
            for (const auto change : batch.changes) {
                if (!batch.connection->connected()) break;
                batch.connection->send(encode(sequence++, change));
            }
            if (batch.connection->connected()) batch.connection->send(batch.token, drogon::WebSocketMessageType::Ping);
        } catch (...) {
            remove(batch.connection);
            closeConnection(batch.connection);
        }
    }
}

void EventHub::close() noexcept {
    decltype(subscribers_) connections;
    {
        std::lock_guard lock(subscribersMutex_);
        closed_ = true;
        connections.swap(subscribers_);
    }
    for (const auto& entry : connections) closeConnection(entry.first);
}

}
