#pragma once

#include <string>
#include <sys/socket.h>
#include <unistd.h>

constexpr int DEFAULT_PORT = 5555;
constexpr size_t MAX_NAME_LEN = 20;
constexpr size_t MAX_MSG_LEN = 1024;

// Limits that keep the server healthy when it is reachable from the internet.
constexpr int MAX_CONNECTIONS = 100;        // total sockets, including ones still picking a name
constexpr int MAX_CONNECTIONS_PER_IP = 5;
constexpr int HANDSHAKE_TIMEOUT_SEC = 60;   // time allowed to pick a name
constexpr int MAX_NAME_ATTEMPTS = 5;
constexpr int SEND_TIMEOUT_SEC = 5;         // a client that stops reading gets dropped
constexpr double RATE_BURST = 8.0;          // messages allowed in a quick burst
constexpr double RATE_PER_SEC = 1.0;        // sustained messages per second
constexpr int MAX_RATE_STRIKES = 15;        // dropped messages before a kick

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // macOS: SIGPIPE is ignored via signal() instead
#endif

// Sends the whole string, retrying on partial writes.
inline bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline bool sendLine(int fd, const std::string& line) {
    return sendAll(fd, line + "\n");
}

// Reads newline-delimited messages from a socket. TCP is a byte stream,
// so one recv() may hold half a message or several messages at once.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    // Returns false when the connection is closed or errors.
    bool readLine(std::string& out) {
        while (true) {
            size_t pos = buffer_.find('\n');
            if (pos != std::string::npos) {
                out = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 1);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            if (buffer_.size() > MAX_MSG_LEN * 4) return false; // flood guard

            char chunk[512];
            ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            buffer_.append(chunk, static_cast<size_t>(n));
        }
    }

private:
    int fd_;
    std::string buffer_;
};

// Removes control characters so nobody can send terminal escape codes
// (clearing screens, fake colors) to other users. UTF-8 bytes are kept.
inline std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\t') out += ' ';
        else if (c >= 0x20 && c != 0x7f) out += static_cast<char>(c);
    }
    return out;
}

inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}
