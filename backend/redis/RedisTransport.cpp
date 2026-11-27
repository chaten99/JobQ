#include "RedisTransport.h"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/redis/src.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace jobq::redis {

namespace asio = boost::asio;
namespace client = boost::redis;

namespace {

constexpr std::array keySuffixes{"ids", "records", "ready:high", "ready:normal", "ready:low",
    "delayed", "workers", "claims", "generations", "schema", "probe"};

bool alphanumeric(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

bool validNamespace(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char ch) {
        return alphanumeric(ch) || ch == '-' || ch == '_';
    });
}

bool validDelay(std::chrono::milliseconds value, bool allowZero = false) {
    return value.count() >= (allowZero ? 0 : 1) && value <= std::chrono::minutes(5);
}

Error commandError(std::string_view diagnostic) {
    const auto code = diagnostic.substr(0, diagnostic.find(' '));
    if (code == "WRONGPASS" || code == "NOAUTH" || code == "AUTH") return Error::Authentication;
    if (code == "NOPERM") return Error::Permission;
    if (diagnostic == "NOSCRIPT No matching script. Please use EVAL.") return Error::NoScript;
    if (code == "NOPROTO") return Error::Protocol;
    return Error::Command;
}

bool validSha(std::string_view sha) {
    return sha.size() == 40 && std::ranges::all_of(sha, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::optional<std::string> singleString(const Reply& reply) {
    if (reply.size() != 1 || reply.front().depth != 0 ||
        (reply.front().respType != '+' && reply.front().respType != '$')) return {};
    return reply.front().value;
}

}

void Config::validate() const {
    if (host.empty() || host.size() > 253 || !std::ranges::all_of(host, [](char ch) {
        return alphanumeric(ch) || ch == '.' || ch == '-' || ch == ':';
    })) throw std::invalid_argument("Invalid Redis host");
    if (port == 0 || port > 65535) throw std::invalid_argument("Invalid Redis port");
    if (database < 0) throw std::invalid_argument("Invalid Redis database index");
    if (clientName.empty() || clientName.size() > 128 || !std::ranges::all_of(clientName, [](unsigned char ch) {
        return ch > 32 && ch < 127;
    })) throw std::invalid_argument("Invalid Redis client name");
    if ((username && (username->empty() || !password)) || (password && password->empty()))
        throw std::invalid_argument("Invalid Redis credentials");
    if (!validDelay(connectTimeout) || !validDelay(commandTimeout) || !validDelay(reconnectDelay, true))
        throw std::invalid_argument("Invalid Redis timeout or reconnect delay");
    if (!validNamespace(nameSpace)) throw std::invalid_argument("Invalid Redis namespace");
    if (tls) throw std::invalid_argument("Verified Redis TLS is not supported by this transport");
}

Keyspace::Keyspace(std::string_view nameSpace) {
    if (!validNamespace(nameSpace)) throw std::invalid_argument("Invalid Redis namespace");
    prefix_ = "jobq:{" + std::string(nameSpace) + "}:v1:";
}

std::string Keyspace::key(KeyKind kind) const {
    return prefix_ + keySuffixes.at(static_cast<std::size_t>(kind));
}

bool Keyspace::owns(std::string_view key) const noexcept {
    return key.starts_with(prefix_) && std::ranges::find(keySuffixes, key.substr(prefix_.size())) != keySuffixes.end();
}

struct Transport::Impl {
    using Guard = asio::executor_work_guard<asio::io_context::executor_type>;

    struct Operation {
        client::request request;
        client::generic_response response;
        asio::steady_timer timer;
        asio::cancellation_signal cancel;
        std::promise<Result<Reply>> result;
        bool timedOut{false};

        Operation(asio::io_context& io, std::string_view command, std::span<const std::string> args)
            : timer(io) {
            auto& flags = request.get_config();
            flags.cancel_on_connection_lost = true;
            flags.cancel_if_not_connected = true;
            flags.cancel_if_unresponded = true;
            if (args.empty()) request.push(command);
            else request.push_range(command, args);
        }
    };

    explicit Impl(Config cfg)
        : config(std::move(cfg)), keys(config.nameSpace),
          connection(io, client::logger(client::logger::level::info,
            [this](auto, std::string_view message) {
                constexpr std::string_view failure = "Setup request execution failed: ";
                if (message.starts_with(failure)) {
                    const auto diagnostic = message.substr(failure.size());
                    const auto category = commandError(diagnostic);
                    setupError = category != Error::Command || diagnostic.starts_with("ERR ")
                        ? category : Error::Connection;
                    record(State::Unavailable, setupError);
                } else if (message == "Setup request execution: success") {
                    setupError.reset();
                }
            })) {
        settings.addr.host = config.host;
        settings.addr.port = std::to_string(config.port);
        settings.resolve_timeout = config.connectTimeout;
        settings.connect_timeout = config.connectTimeout;
        settings.reconnect_wait_interval = config.reconnectDelay;
        settings.max_read_size = 4 * 1024 * 1024;
        settings.use_setup = true;
        settings.setup.clear();
        if (config.password) settings.setup.hello_setname(config.username.value_or("default"), *config.password, config.clientName);
        else settings.setup.hello_setname(config.clientName);
        if (config.database != 0) settings.setup.push("SELECT", config.database);
    }

    void record(State state, std::optional<Error> error = {}) {
        std::lock_guard lock(healthMutex);
        if (status.state != State::Stopped) status = {state, error};
    }

    Result<Reply> readReply(Operation& operation, boost::system::error_code ec) {
        if (operation.timedOut) return Error::Timeout;
        if (stopping.load()) return Error::Shutdown;
        if (ec) {
            if (setupError) return *setupError;
            if (ec == client::error::resolve_timeout || ec == client::error::connect_timeout ||
                ec == client::error::write_timeout || ec == client::error::pong_timeout) return Error::Timeout;
            if (ec == client::error::not_connected || ec == asio::error::operation_aborted ||
                ec.category() != client::make_error_code(client::error::invalid_data_type).category()) return Error::Connection;
            return Error::Protocol;
        }
        if (operation.response.has_error()) {
            const auto& error = operation.response.error();
            if (error.data_type == client::resp3::type::null) return Reply{{'_', 0, 0, {}}};
            return commandError(error.diagnostic);
        }
        Reply result;
        for (auto& node : operation.response.value()) {
            if (node.data_type == client::resp3::type::simple_error || node.data_type == client::resp3::type::blob_error)
                return node.depth == 0 ? commandError(node.value) : Error::Command;
            if (node.data_type == client::resp3::type::invalid) return Error::Protocol;
            result.push_back({client::resp3::to_code(node.data_type), node.depth, node.aggregate_size, std::move(node.value)});
        }
        if (result.empty()) return Error::UnexpectedResponse;
        return result;
    }

    void perform(const std::shared_ptr<Operation>& operation) {
        if (stopping.load() || runEnded) {
            operation->result.set_value(stopping.load() ? Error::Shutdown : setupError.value_or(Error::Connection));
            return;
        }
        operation->timer.expires_after(config.commandTimeout);
        operation->timer.async_wait([operation](boost::system::error_code ec) {
            if (ec) return;
            operation->timedOut = true;
            operation->cancel.emit(asio::cancellation_type::terminal);
        });
        connection.async_exec(operation->request, operation->response,
            asio::bind_cancellation_slot(operation->cancel.slot(),
                [this, operation](boost::system::error_code ec, std::size_t) {
                    operation->timer.cancel();
                    auto result = readReply(*operation, ec);
                    if (const auto* error = std::get_if<Error>(&result)) {
                        if (*error == Error::Connection || *error == Error::Timeout ||
                            *error == Error::Authentication || *error == Error::Protocol)
                            record(State::Unavailable, *error);
                        else if (*error != Error::Shutdown) {
                            std::lock_guard lock(healthMutex);
                            if (status.state != State::Stopped) status.lastError = *error;
                        }
                    }
                    operation->result.set_value(std::move(result));
                }));
    }

    Config config;
    Keyspace keys;
    asio::io_context io;
    client::config settings;
    mutable std::mutex healthMutex;
    Health status;
    std::optional<Error> setupError;
    client::connection connection;
    std::mutex lifecycle;
    std::optional<Guard> guard;
    std::thread worker;
    std::atomic<bool> stopping{false};
    bool started{false};
    bool runEnded{false};
};

Transport::Transport(Config config) {
    config.validate();
    impl_ = std::make_unique<Impl>(std::move(config));
}

Transport::~Transport() { stop(); }

bool Transport::start() {
    auto& runtime = *impl_;
    std::lock_guard lock(runtime.lifecycle);
    if (runtime.started || runtime.stopping.load()) return false;
    runtime.guard.emplace(asio::make_work_guard(runtime.io));
    runtime.record(State::Connecting);
    try {
        runtime.worker = std::thread([&runtime] { runtime.io.run(); });
        asio::post(runtime.io, [&runtime] {
            runtime.connection.async_run(runtime.settings, [&runtime](boost::system::error_code ec) {
                runtime.runEnded = true;
                if (!runtime.stopping.load())
                    runtime.record(State::Unavailable, runtime.setupError.value_or(ec ? Error::Connection : Error::Shutdown));
            });
        });
    } catch (...) {
        runtime.guard.reset();
        if (runtime.worker.joinable()) runtime.worker.join();
        runtime.stopping.store(true);
        runtime.record(State::Stopped, Error::Connection);
        throw std::runtime_error("Unable to start Redis transport");
    }
    runtime.started = true;
    return true;
}

void Transport::stop() {
    auto& runtime = *impl_;
    std::lock_guard lock(runtime.lifecycle);
    if (runtime.stopping.exchange(true)) return;
    if (runtime.worker.joinable()) {
        asio::post(runtime.io, [&runtime] {
            runtime.connection.cancel();
            runtime.guard.reset();
        });
        runtime.worker.join();
    }
    runtime.record(State::Stopped);
}

Health Transport::health() const {
    std::lock_guard lock(impl_->healthMutex);
    return impl_->status;
}

Result<Reply> Transport::execute(std::string_view command, std::span<const std::string> arguments) {
    if (command.empty() || !std::ranges::all_of(command, [](char ch) { return alphanumeric(ch) || ch == '_'; }))
        return Error::InvalidInput;
    auto& runtime = *impl_;
    auto operation = std::make_shared<Impl::Operation>(runtime.io, command, arguments);
    auto result = operation->result.get_future();
    {
        std::lock_guard lock(runtime.lifecycle);
        if (runtime.stopping.load()) return Error::Shutdown;
        if (!runtime.started) return Error::Connection;
        asio::post(runtime.io, [&runtime, operation] { runtime.perform(operation); });
    }
    return result.get();
}

Outcome Transport::ping() {
    const auto result = execute("PING");
    if (const auto* error = std::get_if<Error>(&result)) {
        if (*error != Error::Shutdown && health().state != State::Idle)
            impl_->record(State::Unavailable, *error);
        return *error;
    }
    if (singleString(std::get<Reply>(result)) != "PONG") {
        impl_->record(State::Unavailable, Error::UnexpectedResponse);
        return Error::UnexpectedResponse;
    }
    impl_->record(State::Responsive);
    return std::monostate{};
}

Script::Script(std::string source, std::string sha)
    : source_(std::move(source)), sha_(std::move(sha)) {}

Result<Script> Transport::loadScript(std::string_view source) {
    if (source.empty()) return Error::InvalidInput;
    const std::array args{std::string("LOAD"), std::string(source)};
    auto loaded = execute("SCRIPT", args);
    if (const auto* error = std::get_if<Error>(&loaded)) return *error;
    auto sha = singleString(std::get<Reply>(loaded));
    if (!sha || !validSha(*sha)) return Error::UnexpectedResponse;
    return Script(std::string(source), std::move(*sha));
}

Result<Reply> Transport::eval(const Script& script, std::span<const std::string> keys,
                             std::span<const std::string> arguments) {
    if (!std::ranges::all_of(keys, [this](const auto& key) { return impl_->keys.owns(key); }))
        return Error::InvalidInput;
    std::vector<std::string> args{script.sha_, std::to_string(keys.size())};
    args.insert(args.end(), keys.begin(), keys.end());
    args.insert(args.end(), arguments.begin(), arguments.end());
    auto result = execute("EVALSHA", args);
    if (const auto* error = std::get_if<Error>(&result); error && *error == Error::NoScript) {
        auto reloaded = loadScript(script.source_);
        if (const auto* reloadError = std::get_if<Error>(&reloaded)) return *reloadError;
        if (std::get<Script>(reloaded).sha() != script.sha()) return Error::UnexpectedResponse;
        return execute("EVALSHA", args);
    }
    return result;
}

}
