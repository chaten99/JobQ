#include <winsock2.h>
#include <ws2tcpip.h>

#include "ServerRuntime.h"
#include <sstream>
#include <stdexcept>

int main() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) { WSACleanup(); return 1; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return 1;
    int size = sizeof(address);
    getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size);
    closesocket(socket);
    socket = INVALID_SOCKET;
    jobq::server::ServerConfig config;
    config.port = ntohs(address.sin_port);
    jobq::Engine* acquired = nullptr;
    int result = 1;
    try {
        jobq::server::ServerRuntime runtime(config, [&](jobq::Engine& engine) {
            acquired = &engine;
            socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            BOOL exclusive = TRUE;
            setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
            if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(socket, 1) != 0) {
                throw std::runtime_error("Fixture reservation failed");
            }
        });
        std::ostringstream output, errors;
        const auto status = runtime.run(output, errors);
        result = status == 1 && acquired && acquired->snapshot().workersStopped &&
                 errors.str().find("Listener bind failed") != std::string::npos && !runtime.ready() ? 0 : 1;
    } catch (...) { }
    if (socket != INVALID_SOCKET) closesocket(socket);
    WSACleanup();
    return result;
}
