#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jobq::redis {

struct Config {
    std::string host{"127.0.0.1"};
    unsigned port{6379};
    std::optional<std::string> username;
    std::optional<std::string> password;
    int database{0};
    std::string clientName{"jobq"};
    std::chrono::milliseconds connectTimeout{1000};
    std::chrono::milliseconds commandTimeout{1000};
    std::chrono::milliseconds reconnectDelay{250};
    std::string nameSpace{"default"};
    bool tls{false};

    void validate() const;
};

enum class KeyKind { JobIdSequence, Records, ReadyHigh, ReadyNormal, ReadyLow, Delayed, Workers, Claims, Generations, Schema, Probe };

class Keyspace {
public:
    explicit Keyspace(std::string_view nameSpace);
    std::string key(KeyKind kind) const;
    bool owns(std::string_view key) const noexcept;

private:
    std::string prefix_;
};

enum class Error { Connection, Authentication, Permission, Timeout, Command, Protocol, Shutdown, UnexpectedResponse, NoScript, InvalidInput };
enum class State { Idle, Connecting, Responsive, Unavailable, Stopped };

struct Health {
    State state{State::Idle};
    std::optional<Error> lastError;
};

template<class T>
using Result = std::variant<T, Error>;
using Outcome = Result<std::monostate>;

struct ReplyNode {
    char respType;
    std::size_t depth;
    std::size_t aggregateSize;
    std::string value;
};
using Reply = std::vector<ReplyNode>;

class Script {
public:
    std::string_view sha() const noexcept { return sha_; }

private:
    friend class Transport;
    Script(std::string source, std::string sha);
    std::string source_;
    std::string sha_;
};

class Transport {
public:
    explicit Transport(Config config);
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool start();
    void stop();
    Outcome ping();
    Health health() const;
    Result<Reply> execute(std::string_view command, std::span<const std::string> arguments = {});
    Result<Script> loadScript(std::string_view source);
    Result<Reply> eval(const Script& script, std::span<const std::string> keys = {},
                       std::span<const std::string> arguments = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
