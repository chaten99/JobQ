#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>

#include <json/json.h>

namespace
{

    struct Handle
    {
        HANDLE value{nullptr};
        ~Handle()
        {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    };
    struct Socket
    {
        SOCKET value{INVALID_SOCKET};
        ~Socket()
        {
            if (value != INVALID_SOCKET)
                closesocket(value);
        }
    };
    void ensure(bool ok, const std::string &message)
    {
        if (!ok)
            throw std::runtime_error(message);
    }
    BOOL WINAPI ignoreControl(DWORD) { return TRUE; }

    std::string request(unsigned port, const std::string &path, const std::string &body = {})
    {
        Socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        ensure(socket.value != INVALID_SOCKET, "Create HTTP socket");
        DWORD timeout = 2000;
        setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        setsockopt(socket.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ensure(connect(socket.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "Connect HTTP socket");
        const auto text = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
        ensure(send(socket.value, text.data(), static_cast<int>(text.size()), 0) == static_cast<int>(text.size()), "Send health request");
        std::string result;
        char buffer[4096];
        int count;
        while ((count = recv(socket.value, buffer, sizeof(buffer), 0)) > 0)
            result.append(buffer, count);
        ensure(count == 0, "Read complete HTTP response");
        return result;
    }

    void validateHealth(const std::string &response, const std::string &expected)
    {
        ensure(response.find("200 OK") != std::string::npos, "Health status must be 200: " + response);
        ensure(response.find("application/json") != std::string::npos, "Health content type must be JSON");
        const auto separator = response.find("\r\n\r\n");
        ensure(separator != std::string::npos, "Response must include a body");
        const auto body = response.substr(separator + 4);
        Json::CharReaderBuilder builder;
        auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
        Json::Value json;
        std::string errors;
        ensure(reader->parse(body.data(), body.data() + body.size(), &json, &errors) && json["status"] == expected,
               "Health JSON must contain actual expected status");
    }

    void run(const std::wstring &executable)
    {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle read, write;
        ensure(CreatePipe(&read.value, &write.value, &security, 0), "Create server output pipe");
        SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = write.value;
        startup.hStdError = write.value;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        auto command = L"\"" + executable + L"\" --port 0 --http-threads 2 --engine-workers 2 --max-body-bytes 32";
        ensure(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NEW_CONSOLE,
                              nullptr, nullptr, &startup, &process),
               "Start server subprocess");
        Handle child{process.hProcess}, thread{process.hThread};
        CloseHandle(write.value);
        write.value = nullptr;
        bool joined = false;
        std::string output;
        auto readAvailable = [&]
        {
            DWORD available = 0;
            while (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available)
            {
                char buffer[4096];
                DWORD count = 0;
                if (!ReadFile(read.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr))
                    break;
                output.append(buffer, count);
            }
        };
        try
        {
            const auto deadline = GetTickCount64() + 10000;
            std::smatch match;
            const std::regex ready("JobQ HTTP server ready on 127.0.0.1:([0-9]+)");
            while (GetTickCount64() < deadline)
            {
                readAvailable();
                if (std::regex_search(output, match, ready))
                    break;
                if (WaitForSingleObject(child.value, 10) == WAIT_OBJECT_0)
                    break;
            }
            ensure(std::regex_search(output, match, ready), "Server never reported actual readiness: " + output);
            const auto port = static_cast<unsigned>(std::stoul(match[1].str()));
            ensure(port != 0, "Ephemeral listener must expose the actual bound port");
            validateHealth(request(port, "/health/live"), "ok");
            validateHealth(request(port, "/health/ready"), "ready");
            ensure(request(port, "/health/live", std::string(64, 'x')).find("413") != std::string::npos,
                   "Configured request body limit must be enforced by Drogon");
            ensure(request(port, "/jobs").find("404") != std::string::npos, "Future job APIs must not be advertised as implemented");
            FreeConsole();
            ensure(AttachConsole(process.dwProcessId), "Attach server console");
            ensure(SetConsoleCtrlHandler(ignoreControl, TRUE), "Protect test controller");
            ensure(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0), "Request server shutdown");
            ensure(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0, "Server did not stop within timeout");
            joined = true;
            readAvailable();
            DWORD exit = 1;
            GetExitCodeProcess(child.value, &exit);
            ensure(exit == 0 && output.find("Engine workers joined.") != std::string::npos,
                   "Server cleanup did not complete successfully: " + output);
            SetConsoleCtrlHandler(ignoreControl, FALSE);
            FreeConsole();
            Socket probe{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
            BOOL exclusive = TRUE;
            setsockopt(probe.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(port));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ensure(bind(probe.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "Server port was not released");
            std::cout << "HTTP subprocess health, JSON, readiness, shutdown, workers, and released port passed.\n";
        }
        catch (...)
        {
            if (!joined)
            {
                TerminateProcess(child.value, 1);
                WaitForSingleObject(child.value, 5000);
            }
            SetConsoleCtrlHandler(ignoreControl, FALSE);
            FreeConsole();
            throw;
        }
    }

}

int main(int argc, char **argv)
{
    WSADATA data;
    if (argc != 2 || WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return 1;
    int result = 0;
    try
    {
        const std::string path = argv[1];
        run(std::wstring(path.begin(), path.end()));
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    WSACleanup();
    return result;
}
