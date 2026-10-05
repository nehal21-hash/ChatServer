// ChatServer - client
// Connects to the server, picks a name, then sends what you type
// and prints everything the global chat says.

#include "common.h"

#include <netdb.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

static std::mutex outputMutex;
static std::atomic<bool> running{true};

static const char* PROMPT = "> ";

// Prints an incoming line without mangling the "> " prompt.
static void printIncoming(const std::string& line) {
    std::lock_guard<std::mutex> lock(outputMutex);
    std::string colored = line;
    if (line.rfind("***", 0) == 0) colored = "\033[33m" + line + "\033[0m";          // system: yellow
    else if (line.find("(whisper") != std::string::npos) colored = "\033[35m" + line + "\033[0m"; // whisper: magenta
    else if (line.rfind("* ", 0) == 0) colored = "\033[36m" + line + "\033[0m";     // /me: cyan
    std::cout << "\r\033[K" << colored << "\n" << PROMPT << std::flush;
}

static int connectTo(const std::string& host, const std::string& port) {
    addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        std::cerr << "Cannot resolve " << host << ": " << gai_strerror(rc) << std::endl;
        return -1;
    }

    int fd = -1;
    for (addrinfo* p = result; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    // Priority: command-line args, then CHAT_HOST / CHAT_PORT, then localhost.
    const char* envHost = std::getenv("CHAT_HOST");
    const char* envPort = std::getenv("CHAT_PORT");
    std::string host = argc > 1 ? argv[1] : envHost ? envHost : "127.0.0.1";
    std::string port = argc > 2 ? argv[2] : envPort ? envPort : std::to_string(DEFAULT_PORT);

    // Accept "host:port" (e.g. bore.pub:6094) and "[ipv6]:port" in one argument.
    if (argc <= 2) {
        size_t colon = host.rfind(':');
        if (host.size() > 2 && host.front() == '[' && host.find("]:") != std::string::npos) {
            size_t close = host.find("]:");
            port = host.substr(close + 2);
            host = host.substr(1, close - 1);
        } else if (colon != std::string::npos && host.find(':') == colon) { // exactly one ':'
            port = host.substr(colon + 1);
            host = host.substr(0, colon);
        }
    }

    int fd = connectTo(host, port);
    if (fd < 0) {
        std::cerr << "Could not connect to " << host << ":" << port << std::endl;
        return 1;
    }
    std::cout << "Connected to " << host << ":" << port << std::endl;

    LineReader reader(fd);
    std::string line;

    // Pick a name until the server accepts it.
    std::string name;
    while (true) {
        std::cout << "Enter your name: " << std::flush;
        if (!std::getline(std::cin, name)) { ::close(fd); return 0; }
        name = trim(name);
        if (!sendLine(fd, name) || !reader.readLine(line)) {
            std::cerr << "Lost connection to server." << std::endl;
            ::close(fd);
            return 1;
        }
        if (line.rfind("OK ", 0) == 0) { name = line.substr(3); break; }
        std::cout << "\033[31m" << (line.rfind("ERR ", 0) == 0 ? line.substr(4) : line)
                  << "\033[0m" << std::endl;
    }

    // Receive in the background while the main thread reads the keyboard.
    std::thread receiver([&reader] {
        std::string msg;
        while (reader.readLine(msg)) printIncoming(msg);
        if (running) {
            std::lock_guard<std::mutex> lock(outputMutex);
            std::cout << "\r\033[K\033[31mDisconnected from server.\033[0m" << std::endl;
            std::_Exit(0); // main thread is blocked on getline; just end the process
        }
    });

    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << PROMPT << std::flush;
    }

    std::string input;
    while (std::getline(std::cin, input)) {
        {
            // Erase the typed line; the server echoes it back formatted.
            std::lock_guard<std::mutex> lock(outputMutex);
            std::cout << "\033[A\r\033[K" << PROMPT << std::flush;
        }
        input = trim(input);
        if (input.empty()) continue;
        if (!sendLine(fd, input)) break;
        if (input == "/quit" || input == "/exit") break;
    }

    running = false;
    ::shutdown(fd, SHUT_RDWR);
    receiver.join();
    ::close(fd);
    std::cout << "\nBye!" << std::endl;
    return 0;
}
