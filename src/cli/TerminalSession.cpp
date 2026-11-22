#include "cli/TerminalSession.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace jobq::cli {

namespace {

std::atomic<bool> sessionActive{false};
std::atomic<int> pendingSignal{0};
static_assert(std::atomic<int>::is_always_lock_free);

extern "C" void recordSignal(int number) {
    int expected = 0;
    pendingSignal.compare_exchange_strong(expected, number, std::memory_order_relaxed);
}

#ifdef _WIN32
BOOL WINAPI consoleControl(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        int expected = 0;
        pendingSignal.compare_exchange_strong(expected,
            event == CTRL_C_EVENT ? SIGINT : SIGBREAK, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}
#endif

}

struct TerminalSession::Impl {
    bool registered{false};
    bool interruptInstalled{false};
    bool terminateInstalled{false};
    bool pipeInstalled{false};
    std::string pending;
    bool eof{false};
    bool consoleLineReady{false};
#ifdef _WIN32
    using Handler = void (*)(int);
    Handler interruptHandler{SIG_DFL};
    Handler terminateHandler{SIG_DFL};
    HANDLE input{GetStdHandle(STD_INPUT_HANDLE)};
#else
    struct sigaction interruptHandler{};
    struct sigaction terminateHandler{};
    struct sigaction pipeHandler{};
#endif
};

TerminalSession::TerminalSession() : impl_(std::make_unique<Impl>()) {
    bool expected = false;
    if (!sessionActive.compare_exchange_strong(expected, true)) {
        throw std::runtime_error("Only one terminal signal session can be active");
    }
    pendingSignal.store(0);
    try {
#ifdef _WIN32
        impl_->interruptHandler = std::signal(SIGINT, recordSignal);
        if (impl_->interruptHandler == SIG_ERR) throw std::runtime_error("Unable to register SIGINT");
        impl_->interruptInstalled = true;
        impl_->terminateHandler = std::signal(SIGTERM, recordSignal);
        if (impl_->terminateHandler == SIG_ERR) throw std::runtime_error("Unable to register SIGTERM");
        impl_->terminateInstalled = true;
        if (!SetConsoleCtrlHandler(consoleControl, TRUE)) {
            throw std::system_error(GetLastError(), std::system_category(), "Unable to register console handler");
        }
        impl_->registered = true;
#else
        struct sigaction action{};
        action.sa_handler = recordSignal;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGINT);
        sigaddset(&action.sa_mask, SIGTERM);
        if (sigaction(SIGINT, &action, &impl_->interruptHandler) != 0) {
            throw std::system_error(errno, std::generic_category(), "Unable to register SIGINT");
        }
        impl_->interruptInstalled = true;
        if (sigaction(SIGTERM, &action, &impl_->terminateHandler) != 0) {
            throw std::system_error(errno, std::generic_category(), "Unable to register SIGTERM");
        }
        impl_->terminateInstalled = true;
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        if (sigaction(SIGPIPE, &ignore, &impl_->pipeHandler) != 0) {
            throw std::system_error(errno, std::generic_category(), "Unable to register SIGPIPE disposition");
        }
        impl_->pipeInstalled = true;
#endif
    } catch (const std::exception& original) {
        const std::string message = original.what();
        try { restore(); }
        catch (const std::exception& cleanup) {
            throw std::runtime_error(message + "; signal cleanup failed: " + cleanup.what());
        }
        throw;
    }
}

TerminalSession::~TerminalSession() noexcept {
    if (impl_->registered || impl_->interruptInstalled || impl_->terminateInstalled || impl_->pipeInstalled) {
        try { restore(); }
        catch (const std::exception& error) { std::fprintf(stderr, "Signal cleanup failure: %s\n", error.what()); }
        catch (...) { std::fputs("Signal cleanup failure: unknown error.\n", stderr); }
    }
}

void TerminalSession::restore() {
    bool failed = false;
#ifdef _WIN32
    if (impl_->registered) {
        if (SetConsoleCtrlHandler(consoleControl, FALSE)) impl_->registered = false;
        else failed = true;
    }
    if (impl_->interruptInstalled) {
        if (std::signal(SIGINT, impl_->interruptHandler) != SIG_ERR) impl_->interruptInstalled = false;
        else failed = true;
    }
    if (impl_->terminateInstalled) {
        if (std::signal(SIGTERM, impl_->terminateHandler) != SIG_ERR) impl_->terminateInstalled = false;
        else failed = true;
    }
#else
    if (impl_->interruptInstalled) {
        if (sigaction(SIGINT, &impl_->interruptHandler, nullptr) == 0) impl_->interruptInstalled = false;
        else failed = true;
    }
    if (impl_->terminateInstalled) {
        if (sigaction(SIGTERM, &impl_->terminateHandler, nullptr) == 0) impl_->terminateInstalled = false;
        else failed = true;
    }
    if (impl_->pipeInstalled) {
        if (sigaction(SIGPIPE, &impl_->pipeHandler, nullptr) == 0) impl_->pipeInstalled = false;
        else failed = true;
    }
#endif
    if (failed) throw std::runtime_error("Unable to restore terminal signal handlers");
    sessionActive.store(false);
}

int TerminalSession::signalNumber() const noexcept {
    if (!impl_->registered && !impl_->interruptInstalled && !impl_->terminateInstalled) return 0;
    return pendingSignal.load(std::memory_order_relaxed);
}

bool TerminalSession::readLine(std::string& line, const std::function<bool()>& cancelled,
                               const std::function<void()>& wait) {
    while (!cancelled()) {
        const auto newline = impl_->pending.find('\n');
        if (newline != std::string::npos || impl_->eof) {
            if (impl_->pending.empty() && impl_->eof) return false;
            line = impl_->pending.substr(0, newline);
            impl_->pending.erase(0, newline == std::string::npos ? impl_->pending.size() : newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        char buffer[4096];
        std::size_t bytes = 0;
#ifdef _WIN32
        DWORD mode = 0;
        const bool console = GetConsoleMode(impl_->input, &mode) != FALSE;
        if (console && !impl_->consoleLineReady) {
            DWORD count = 0;
            if (!GetNumberOfConsoleInputEvents(impl_->input, &count)) {
                throw std::runtime_error("Unable to inspect terminal input");
            }
            std::vector<INPUT_RECORD> events(count);
            DWORD available = 0;
            if (count && !PeekConsoleInputW(impl_->input, events.data(), count, &available)) {
                throw std::runtime_error("Unable to inspect console input");
            }
            const bool complete = std::any_of(events.begin(), events.begin() + available, [](const auto& event) {
                return event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown &&
                       event.Event.KeyEvent.uChar.UnicodeChar == L'\r';
            });
            if (!complete) { wait(); continue; }
            impl_->consoleLineReady = true;
        } else if (GetFileType(impl_->input) == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(impl_->input, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) { impl_->eof = true; continue; }
                throw std::runtime_error("Unable to inspect piped input");
            }
            if (!available) { wait(); continue; }
        }
        DWORD read = 0;
        if (!ReadFile(impl_->input, buffer, sizeof(buffer), &read, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) impl_->eof = true;
            else if (error == ERROR_OPERATION_ABORTED && cancelled()) return false;
            else throw std::system_error(error, std::system_category(), "Unable to read terminal input");
        }
        bytes = read;
        if (console && bytes && buffer[0] == '\x1a') { bytes = 0; impl_->eof = true; }
        if (console && std::find(buffer, buffer + bytes, '\n') != buffer + bytes) impl_->consoleLineReady = false;
#else
        (void)wait;
        struct pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = poll(&input, 1, 25);
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "Unable to poll terminal input");
        }
        if (!ready) continue;
        const auto read = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (read < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            throw std::system_error(errno, std::generic_category(), "Unable to read terminal input");
        }
        bytes = static_cast<std::size_t>(read);
#endif
        if (!bytes) impl_->eof = true;
        else impl_->pending.append(buffer, bytes);
    }
    return false;
}

}
