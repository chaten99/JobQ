#include "RedisTransport.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

namespace {

using namespace jobq::redis;
using namespace std::chrono_literals;

void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

template<class T>
T value(Result<T> result, const char* description) {
    if (auto* success = std::get_if<T>(&result)) return std::move(*success);
    std::cerr << description << " error category " << static_cast<int>(std::get<Error>(result)) << '\n';
    throw std::runtime_error(description);
}

template<class T>
bool fails(const Result<T>& result, Error error) {
    const auto* failure = std::get_if<Error>(&result);
    return failure && *failure == error;
}

template<class Test>
void invalid(Test test) {
    try { test(); }
    catch (const std::invalid_argument& error) {
        require(std::string_view(error.what()).find("sensitive-test-marker") == std::string_view::npos,
                "Validation error exposed credential");
        return;
    }
    throw std::runtime_error("Invalid configuration accepted");
}

template<class Operation>
bool eventually(Operation operation, std::chrono::seconds timeout = 6s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (operation()) return true;
        std::this_thread::sleep_for(25ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void local() {
    Config config;
    config.validate();
    require(config.port == 6379 && config.database == 0, "Unexpected default config");
    auto reject = [&](auto change) {
        auto bad = config;
        change(bad);
        invalid([&] { bad.validate(); });
    };
    reject([](auto& cfg) { cfg.host = ""; });
    reject([](auto& cfg) { cfg.host = "redis://sensitive-test-marker"; });
    reject([](auto& cfg) { cfg.port = 0; });
    reject([](auto& cfg) { cfg.port = 65536; });
    reject([](auto& cfg) { cfg.database = -1; });
    reject([](auto& cfg) { cfg.nameSpace = "bad}:namespace"; });
    reject([](auto& cfg) { cfg.connectTimeout = 0ms; });
    reject([](auto& cfg) { cfg.commandTimeout = 301s; });
    reject([](auto& cfg) { cfg.reconnectDelay = -1ms; });
    reject([](auto& cfg) { cfg.username = "sensitive-test-marker"; });
    reject([](auto& cfg) { cfg.password = ""; });
    reject([](auto& cfg) { cfg.tls = true; cfg.password = "sensitive-test-marker"; });
    Config binaryCredentials = config;
    binaryCredentials.password = std::string("a\0b", 3);
    binaryCredentials.validate();
    constexpr std::array categories{KeyKind::JobIdSequence, KeyKind::Records, KeyKind::ReadyHigh,
        KeyKind::ReadyNormal, KeyKind::ReadyLow, KeyKind::Delayed, KeyKind::Workers,
        KeyKind::Claims, KeyKind::Generations, KeyKind::Schema, KeyKind::Probe};
    constexpr std::array suffixes{"ids", "records", "ready:high", "ready:normal", "ready:low",
        "delayed", "workers", "claims", "generations", "schema", "probe"};
    Keyspace one("north-1"), same("north-1"), two("south");
    for (std::size_t i = 0; i < categories.size(); ++i) {
        const auto key = one.key(categories[i]);
        require(key == "jobq:{north-1}:v1:" + std::string(suffixes[i]) &&
                key == same.key(categories[i]) && !two.owns(key) && one.owns(key), "Incorrect keyspace");
    }
    invalid([] { Keyspace invalid(""); });
    invalid([] { Keyspace invalid("north}{south"); });

    Transport neverStarted(config);
    require(neverStarted.health().state == State::Idle && fails(neverStarted.ping(), Error::Connection), "Idle transport is not healthy");
    neverStarted.stop();
    require(!neverStarted.start(), "Stopped transport restarted");

    config.port = 1;
    config.commandTimeout = 100ms;
    config.connectTimeout = 100ms;
    config.reconnectDelay = 0ms;
    Transport missing(config);
    require(missing.start() && !missing.start(), "Transport starts exactly once");
    require(fails(missing.ping(), Error::Connection), "Connection failure was not reported");
    missing.stop();
    missing.stop();
    require(missing.health().state == State::Stopped && fails(missing.ping(), Error::Shutdown), "Stopped transport accepts commands");
}

std::optional<std::string> environment(const char* name) {
    if (const auto* text = std::getenv(name); text && *text) return std::string(text);
    return {};
}

int number(const std::string& source) {
    int parsed{};
    const auto [end, error] = std::from_chars(source.data(), source.data() + source.size(), parsed);
    if (error != std::errc{} || end != source.data() + source.size()) throw std::invalid_argument("Invalid numeric Redis test setting");
    return parsed;
}

Config liveConfig(std::string host) {
    Config config;
    config.host = std::move(host);
    if (auto port = environment("JOBQ_REDIS_TEST_PORT")) config.port = static_cast<unsigned>(number(*port));
    if (auto db = environment("JOBQ_REDIS_TEST_DATABASE")) config.database = number(*db);
    config.username = environment("JOBQ_REDIS_TEST_USERNAME");
    config.password = environment("JOBQ_REDIS_TEST_PASSWORD");
    std::random_device entropy;
    config.nameSpace = "test-" + std::to_string(entropy()) + "-" + std::to_string(entropy());
    config.commandTimeout = 300ms;
    return config;
}

void live(Config config) {
    auto other = config;
    other.nameSpace += "-other";
    Transport transport(config);
    require(transport.start(), "Live transport did not start");
    require(eventually([&] { return std::holds_alternative<std::monostate>(transport.ping()); }), "Expected real Redis PONG");
    require(transport.health().state == State::Responsive, "PONG not reflected in health");
    const std::array keys{Keyspace(config.nameSpace).key(KeyKind::Probe)};
    const std::array foreign{Keyspace(other.nameSpace).key(KeyKind::Probe)};
    struct Cleanup {
        Transport& transport;
        const std::array<std::string, 1>& keys;
        bool owned{false};
        ~Cleanup() {
            if (owned) {
                try { transport.execute("DEL", keys); } catch (...) {}
            }
        }
    } cleanup{transport, keys};

    const std::string binary("a\0b\r\nc", 6);
    const std::array setArgs{keys[0], binary, std::string("NX"), std::string("PX"), std::string("60000")};
    const auto set = value(transport.execute("SET", setArgs), "SET");
    require(set.size() == 1 && set.front().value == "OK", "SET did not acknowledge");
    cleanup.owned = true;
    const auto get = value(transport.execute("GET", keys), "GET");
    require(get.size() == 1 && get.front().value == binary, "Binary SET/GET distorted bytes");
    const auto absent = value(transport.execute("GET", foreign), "GET inactive namespace");
    require(absent.size() == 1 && absent.front().respType == '_', "Namespace crossed");
    require(fails(transport.execute("UNKNOWNJOBQCOMMAND"), Error::Command), "Command error was lost");
    const auto script = value(transport.loadScript("return {KEYS[1], ARGV[1], 42}"), "SCRIPT LOAD");
    require(script.sha().size() == 40 && fails(transport.eval(script, foreign), Error::InvalidInput), "Script identity or key validation failed");
    const std::array scriptArgs{binary};
    const auto response = value(transport.eval(script, keys, scriptArgs), "EVALSHA");
    require(response.size() == 4 && response[0].aggregateSize == 3 && response[1].value == keys[0] &&
            response[2].value == binary && response[3].value == "42", "Unexpected script reply shape");

    std::array<std::future<Outcome>, 8> requests;
    for (auto& request : requests) request = std::async(std::launch::async, [&] { return transport.ping(); });
    for (auto& request : requests) value(request.get(), "Concurrent PING");
    value(transport.execute("DEL", keys), "DEL temporary key");
    cleanup.owned = false;

    Transport blocked(config);
    require(blocked.start() && eventually([&] { return std::holds_alternative<std::monostate>(blocked.ping()); }), "Timeout connection unavailable");
    const std::array blockArgs{keys[0], std::string("1")};
    require(fails(blocked.execute("BLPOP", blockArgs), Error::Timeout), "Blocking command did not time out");
    blocked.stop();

    config.commandTimeout = 10s;
    config.clientName = config.nameSpace;
    Transport pending(config);
    require(pending.start() && eventually([&] { return std::holds_alternative<std::monostate>(pending.ping()); }), "Shutdown connection unavailable");
    const std::array longBlock{keys[0], std::string("10")};
    auto interrupted = std::async(std::launch::async, [&] { return pending.execute("BLPOP", longBlock); });
    const std::array clientList{std::string("LIST")};
    const bool entered = eventually([&] {
        auto clients = transport.execute("CLIENT", clientList);
        const auto* reply = std::get_if<Reply>(&clients);
        if (!reply || reply->size() != 1) return false;
        std::string_view rows = reply->front().value;
        while (!rows.empty()) {
            const auto newline = rows.find('\n');
            const auto row = rows.substr(0, newline);
            if (row.find(" name=" + config.clientName + " ") != std::string_view::npos &&
                row.find(" cmd=blpop ") != std::string_view::npos) return true;
            if (newline == std::string_view::npos) break;
            rows.remove_prefix(newline + 1);
        }
        return false;
    });
    pending.stop();
    require(entered && fails(interrupted.get(), Error::Shutdown), "Outstanding command not cancelled");

    config.username = config.nameSpace;
    config.password = config.nameSpace;
    config.reconnectDelay = 0ms;
    config.commandTimeout = 300ms;
    Transport rejected(config);
    require(rejected.start() && eventually([&] { return fails(rejected.ping(), Error::Authentication); }),
            "Invalid Redis credentials did not produce a redacted authentication error");
}

void recovery(Config config) {
    Transport transport(config);
    require(transport.start() && eventually([&] { return std::holds_alternative<std::monostate>(transport.ping()); }), "Initial Redis unavailable");
    const auto script = value(transport.loadScript("return 42"), "Load recovery script");
    std::cout << "READY" << std::endl;
    require(eventually([&] {
        const auto ping = transport.ping();
        return (fails(ping, Error::Connection) || fails(ping, Error::Timeout)) &&
               transport.health().state == State::Unavailable;
    }, 15s), "Outage was not observed");
    std::cout << "OUTAGE" << std::endl;
    require(eventually([&] { return std::holds_alternative<std::monostate>(transport.ping()); }, 20s), "Reconnect was not observed");
    const auto reply = value(transport.eval(script), "Reload missing script after restart");
    require(reply.size() == 1 && reply[0].value == "42", "Reconnected script reply did not match");
    transport.stop();
    std::cout << "RECOVERED" << std::endl;
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            local();
        } else {
            auto host = environment("JOBQ_REDIS_TEST_HOST");
            if (!host) {
                std::cout << "SKIP: JOBQ_REDIS_TEST_HOST is not configured\n";
                return 77;
            }
            auto config = liveConfig(*host);
            if (std::string_view(argv[1]) == "--recovery") recovery(std::move(config));
            else live(std::move(config));
        }
        std::cout << "Redis checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
