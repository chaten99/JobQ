#pragma once

#include <functional>
#include <memory>
#include <string>

namespace jobq::cli {

class TerminalSession {
public:
    TerminalSession();
    ~TerminalSession() noexcept;
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;
    int signalNumber() const noexcept;
    bool readLine(std::string& line, const std::function<bool()>& cancelled,
                  const std::function<void()>& wait);
    void restore();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
