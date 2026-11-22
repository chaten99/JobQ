#include "ServerRuntime.h"
#include "EventHub.h"
#include "EventRoutes.h"
#include "HealthRoutes.h"
#include "InspectionRoutes.h"
#include "ApiRoutes.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <drogon/drogon.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jobq::server
{

    namespace
    {

        std::atomic<int> signalRequest{0};
        std::atomic<bool> frameworkClaimed{false};
        static_assert(std::atomic<int>::is_always_lock_free);
        extern "C" void notifySignal(int number) { signalRequest.store(number, std::memory_order_relaxed); }
#ifdef _WIN32
        BOOL WINAPI consoleSignal(DWORD event)
        {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
                return FALSE;
            signalRequest.store(SIGINT, std::memory_order_relaxed);
            return TRUE;
        }
#endif

        class SignalScope
        {
        public:
            SignalScope()
            {
                signalRequest.store(0);
#ifdef _WIN32
                previousInt_ = std::signal(SIGINT, notifySignal);
                if (previousInt_ == SIG_ERR)
                    throw std::runtime_error("Unable to register SIGINT");
                previousTerm_ = std::signal(SIGTERM, notifySignal);
                if (previousTerm_ == SIG_ERR)
                {
                    std::signal(SIGINT, previousInt_);
                    throw std::runtime_error("Unable to register SIGTERM");
                }
                if (!SetConsoleCtrlHandler(consoleSignal, TRUE))
                {
                    std::signal(SIGINT, previousInt_);
                    std::signal(SIGTERM, previousTerm_);
                    throw std::runtime_error("Unable to register console control handler");
                }
#else
                struct sigaction action{};
                action.sa_handler = notifySignal;
                sigemptyset(&action.sa_mask);
                if (sigaction(SIGINT, &action, &previousInt_) != 0)
                    throw std::runtime_error("Unable to register SIGINT");
                if (sigaction(SIGTERM, &action, &previousTerm_) != 0)
                {
                    sigaction(SIGINT, &previousInt_, nullptr);
                    throw std::runtime_error("Unable to register SIGTERM");
                }
#endif
            }
            bool restore() noexcept
            {
                if (restored_)
                    return true;
                bool ok = true;
#ifdef _WIN32
                ok = SetConsoleCtrlHandler(consoleSignal, FALSE) != FALSE;
                ok = std::signal(SIGINT, previousInt_) != SIG_ERR && ok;
                ok = std::signal(SIGTERM, previousTerm_) != SIG_ERR && ok;
#else
                ok = sigaction(SIGINT, &previousInt_, nullptr) == 0;
                ok = sigaction(SIGTERM, &previousTerm_, nullptr) == 0 && ok;
#endif
                restored_ = ok;
                return ok;
            }
            ~SignalScope()
            {
                if (!restore())
                    std::cerr << "Signal restoration failed.\n";
            }

        private:
            bool restored_{false};
#ifdef _WIN32
            void (*previousInt_)(int){SIG_DFL};
            void (*previousTerm_)(int){SIG_DFL};
#else
            struct sigaction previousInt_{};
            struct sigaction previousTerm_{};
#endif
        };

        void checkListener(const ServerConfig &config)
        {
#ifdef _WIN32
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error("Winsock initialization failed");
#endif
            const bool ipv6 = config.address.find(':') != std::string::npos;
            const auto socket = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
            if (socket == INVALID_SOCKET)
            {
                WSACleanup();
                throw std::runtime_error("Listener socket creation failed");
            }
            BOOL exclusive = TRUE;
            setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
#else
            if (socket < 0)
                throw std::runtime_error("Listener socket creation failed");
#endif
            sockaddr_storage storage{};
            int size = 0;
            if (ipv6)
            {
                auto &address = reinterpret_cast<sockaddr_in6 &>(storage);
                address.sin6_family = AF_INET6;
                address.sin6_port = htons(config.port);
                inet_pton(AF_INET6, config.address.c_str(), &address.sin6_addr);
                size = sizeof(address);
            }
            else
            {
                auto &address = reinterpret_cast<sockaddr_in &>(storage);
                address.sin_family = AF_INET;
                address.sin_port = htons(config.port);
                inet_pton(AF_INET, config.address.c_str(), &address.sin_addr);
                size = sizeof(address);
            }
            const int result = ::bind(socket, reinterpret_cast<sockaddr *>(&storage), size);
#ifdef _WIN32
            const int error = WSAGetLastError();
            closesocket(socket);
            WSACleanup();
#else
            const int error = errno;
            close(socket);
#endif
            if (result != 0)
                throw std::runtime_error("Listener bind validation failed, OS error " + std::to_string(error));
        }

    }

    ServerContext::ServerContext(const ServerConfig &config)
        : events(std::make_shared<EventHub>()),
          engine({.workerCount = config.engineWorkers, .storagePath = config.storagePath},
                 [hub = std::weak_ptr<EventHub>(events)](JobStatusChange change) {
                     if (const auto publisher = hub.lock()) publisher->publish(change);
                 }) {}
    bool ServerContext::ready() const { return listening.load() && !stopping.load() && engine.isRunning(); }

    void ServerContext::markListening()
    {
        readyTicks_.store(std::chrono::steady_clock::now().time_since_epoch().count());
        listening.store(true);
    }

    std::chrono::seconds ServerContext::uptime() const
    {
        using Clock = std::chrono::steady_clock;
        const auto ticks = readyTicks_.load();
        if (ticks == 0)
            return {};
        const auto readyAt = Clock::time_point{Clock::duration{ticks}};
        return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - readyAt);
    }

    ServerRuntime::ServerRuntime(ServerConfig config, std::function<void(Engine &)> initialize)
        : config_(std::move(config)), initialize_(std::move(initialize))
    {
        validateConfig(config_);
        context_ = std::make_shared<ServerContext>(config_);
    }

    JobId ServerContext::submit(std::string type, std::string payload, JobOptions options)
    {
        std::lock_guard lock(admission_);
        if (!ready())
            throw std::logic_error("Server submissions are closed");
        return engine.submit(std::move(type), std::move(payload), options);
    }

    void ServerContext::requestStop()
    {
        {
            std::lock_guard lock(admission_);
            stopping.store(true);
        }
        events->close();
    }
    void ServerRuntime::requestStop() noexcept { context_->requestStop(); }
    bool ServerRuntime::ready() const { return context_->ready(); }

    int ServerRuntime::run(std::ostream &output, std::ostream &errors)
    {
        if (used_.exchange(true))
        {
            errors << "Server runtime can only run once.\n";
            return 1;
        }
        auto &app = drogon::app();
        if (frameworkClaimed.exchange(true))
        {
            errors << "Drogon supports one server runtime per process.\n";
            return 1;
        }
        int result = 0;
        bool configured = false;
        trantor::TimerId pollingTimer = trantor::InvalidTimerId;
        try
        {
            checkListener(config_);
            SignalScope signals;
            if (initialize_)
                initialize_(context_->engine);
            if (context_->stopping.load())
            {
                context_->engine.stop();
                return 0;
            }
            context_->engine.start();
            app.disableSigtermHandling().setThreadNum(config_.httpThreads).setClientMaxBodySize(config_.maxBodyBytes).setClientMaxMemoryBodySize(config_.maxBodyBytes).setMaxConnectionNum(config_.maxConnections).setMaxConnectionNumPerIP(config_.maxConnections).setLogLevel(trantor::Logger::kWarn).setServerHeaderField("").setFileTypes({}).setIdleConnectionTimeout(30).addListener(config_.address, config_.port);
            registerHealthRoutes(context_);
            registerApiRoutes(context_);
            registerInspectionRoutes(context_);
            registerEventRoutes(context_);
            app.setClientMaxWebSocketMessageSize(4096);
            configured = true;
            app.registerBeginningAdvice([&, context = context_]
                                        {
            const auto listeners = app.getListeners();
            const auto port = listeners.empty() ? config_.port : listeners.front().toPort();
            const auto probeAddress = config_.address == "0.0.0.0" ? "127.0.0.1" :
                                      config_.address == "::" ? "::1" : config_.address;
            auto client = std::make_shared<drogon::HttpClientPtr>(drogon::HttpClient::newHttpClient("http://" +
                (probeAddress.find(':') == std::string::npos ? probeAddress : "[" + probeAddress + "]") +
                ":" + std::to_string(port), app.getLoop()));
            auto pending = std::make_shared<bool>(false);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            pollingTimer = app.getLoop()->runEvery(0.025, [&, context, client, pending, port, deadline] {
                if (signalRequest.load() || context->stopping.load()) {
                    context->requestStop();
                    context->listening.store(false);
                    app.quit();
                    return;
                }
                if (context->listening.load()) {
                    try { context->events->flush(); }
                    catch (...) { context->events->close(); }
                    return;
                }
                if (*pending) return;
                if (std::chrono::steady_clock::now() > deadline) {
                    result = 1;
                    try { errors << "Listener readiness probe timed out.\n"; } catch (...) { }
                    context->requestStop();
                    app.quit();
                    return;
                }
                *pending = true;
                auto request = drogon::HttpRequest::newHttpRequest();
                request->setPath("/health/live");
                (*client)->sendRequest(request, [&, context, client, pending, port](drogon::ReqResult status,
                                                                       const drogon::HttpResponsePtr& response) {
                    *pending = false;
                    if (status == drogon::ReqResult::Ok && response && response->statusCode() == drogon::k200OK &&
                        !context->stopping.load()) {
                        context->markListening();
                        client->reset();
                        try {
                            output << "JobQ HTTP server ready on " << config_.address << ':' << port << '\n' << std::flush;
                            if (!output) { result = 1; context->requestStop(); }
                        } catch (...) { result = 1; context->requestStop(); }
                    }
                }, 1.0);
            }); });
            app.run();
            if (pollingTimer != trantor::InvalidTimerId)
                app.getLoop()->invalidateTimer(pollingTimer);
            context_->requestStop();
            context_->listening.store(false);
            context_->engine.stop();
            if (!signals.restore())
            {
                errors << "Signal restoration failed.\n";
                result = 1;
            }
            output << "HTTP server stopped; Engine workers joined.\n"
                   << std::flush;
            if (!output)
                result = 1;
        }
        catch (const std::exception &error)
        {
            context_->requestStop();
            context_->listening.store(false);
            try
            {
                errors << "HTTP runtime failure: " << error.what() << '\n';
            }
            catch (...)
            {
            }
            if (configured && app.isRunning())
                app.quit();
            if (pollingTimer != trantor::InvalidTimerId)
                app.getLoop()->invalidateTimer(pollingTimer);
            try
            {
                context_->engine.stop();
            }
            catch (const std::exception &cleanup)
            {
                try
                {
                    errors << "Engine cleanup failure: " << cleanup.what() << '\n';
                }
                catch (...)
                {
                }
            }
            result = 1;
        }
        catch (...)
        {
            context_->requestStop();
            context_->listening.store(false);
            try
            {
                errors << "HTTP runtime failure: unknown exception.\n";
            }
            catch (...)
            {
            }
            if (configured && app.isRunning())
                app.quit();
            if (pollingTimer != trantor::InvalidTimerId)
                app.getLoop()->invalidateTimer(pollingTimer);
            try
            {
                context_->engine.stop();
            }
            catch (...)
            {
                try
                {
                    errors << "Engine cleanup failure.\n";
                }
                catch (...)
                {
                }
            }
            result = 1;
        }
        return result;
    }

}
