// ChatServer - server
// Accepts many clients, each with a unique name, and relays messages
// to everyone in a single global chat room.

#include "common.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

struct Client {
    int fd;
    std::string name;
    std::string address;
    std::mutex sendMutex; // one writer at a time per socket
    bool closed = false;  // guarded by sendMutex; stops writes to a reused fd
};

// Token bucket: allows short bursts but limits the sustained message rate.
class RateLimiter {
public:
    bool allow() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(RATE_BURST, tokens_ + elapsed * RATE_PER_SEC);
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double tokens_ = RATE_BURST;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

static void setTimeout(int fd, int option, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv));
}

class ChatServer {
public:
    explicit ChatServer(int port) : port_(port) {}

    // Listens on IPv6 and IPv4 at once when possible, else IPv4 only.
    bool start() {
        listenFd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (listenFd_ >= 0) {
            int no = 0, yes = 1;
            setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
            setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_addr = in6addr_any;
            addr.sin6_port = htons(static_cast<uint16_t>(port_));
            if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(listenFd_);
                listenFd_ = -1;
            }
        }

        if (listenFd_ < 0) {
            listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listenFd_ < 0) { perror("socket"); return false; }

            int yes = 1;
            setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(static_cast<uint16_t>(port_));
            if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                perror("bind");
                return false;
            }
        }

        if (::listen(listenFd_, 64) < 0) { perror("listen"); return false; }

        log("Server listening on port " + std::to_string(port_) + " (IPv4 + IPv6)");
        return true;
    }

    void run() {
        while (true) {
            sockaddr_storage clientAddr{};
            socklen_t len = sizeof(clientAddr);
            int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &len);
            if (fd < 0) { perror("accept"); continue; }

            std::string ip = ipOf(clientAddr, len);
            std::string reason = reserveSlot(ip);
            if (!reason.empty()) {
                log("Rejected " + ip + ": " + reason);
                sendLine(fd, "ERR " + reason);
                ::close(fd);
                continue;
            }

            setTimeout(fd, SO_SNDTIMEO, SEND_TIMEOUT_SEC);
            std::string address = ip + " port " + portOf(clientAddr, len);
            std::thread(&ChatServer::handleClient, this, fd, ip, address).detach();
        }
    }

private:
    int port_;
    int listenFd_ = -1;
    std::mutex clientsMutex_;
    std::map<std::string, std::shared_ptr<Client>> clients_; // key: lowercase name

    std::mutex connMutex_;
    int connections_ = 0;
    std::map<std::string, int> connectionsPerIp_;

    static std::string ipOf(const sockaddr_storage& addr, socklen_t len) {
        char host[NI_MAXHOST] = "?";
        getnameinfo(reinterpret_cast<const sockaddr*>(&addr), len, host, sizeof(host),
                    nullptr, 0, NI_NUMERICHOST);
        std::string ip = host;
        if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7); // IPv4 client on a dual-stack socket
        return ip;
    }

    static std::string portOf(const sockaddr_storage& addr, socklen_t len) {
        char serv[NI_MAXSERV] = "?";
        getnameinfo(reinterpret_cast<const sockaddr*>(&addr), len, nullptr, 0,
                    serv, sizeof(serv), NI_NUMERICSERV);
        return serv;
    }

    // Tunnels (bore, playit, ...) hand us every visitor from localhost,
    // so a per-IP limit there would cap the whole tunnel.
    static bool isLoopback(const std::string& ip) {
        return ip == "::1" || ip.rfind("127.", 0) == 0;
    }

    // Returns an error message if the connection must be refused.
    std::string reserveSlot(const std::string& ip) {
        std::lock_guard<std::mutex> lock(connMutex_);
        if (connections_ >= MAX_CONNECTIONS) return "Server is full, try again later.";
        if (!isLoopback(ip) && connectionsPerIp_[ip] >= MAX_CONNECTIONS_PER_IP)
            return "Too many connections from your address.";
        ++connections_;
        ++connectionsPerIp_[ip];
        return "";
    }

    void releaseSlot(const std::string& ip) {
        std::lock_guard<std::mutex> lock(connMutex_);
        --connections_;
        if (--connectionsPerIp_[ip] <= 0) connectionsPerIp_.erase(ip);
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    static std::string timestamp() {
        std::time_t now = std::time(nullptr);
        char buf[8];
        std::strftime(buf, sizeof(buf), "%H:%M", std::localtime(&now));
        return buf;
    }

    static void log(const std::string& msg) {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        std::cout << "[" << timestamp() << "] " << msg << std::endl;
    }

    static std::string validateName(const std::string& name) {
        if (name.empty()) return "Name cannot be empty.";
        if (name.size() > MAX_NAME_LEN)
            return "Name must be at most " + std::to_string(MAX_NAME_LEN) + " characters.";
        for (char c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
                return "Name may only contain letters, digits, '_' and '-'.";
        }
        return "";
    }

    // A client whose socket stops accepting data (send timeout) is shut down,
    // which makes its own thread notice and clean up.
    static void sendTo(Client& c, const std::string& line) {
        std::lock_guard<std::mutex> lock(c.sendMutex);
        if (c.closed) return;
        if (!sendLine(c.fd, line)) ::shutdown(c.fd, SHUT_RDWR);
    }

    std::vector<std::shared_ptr<Client>> snapshot() {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        std::vector<std::shared_ptr<Client>> list;
        for (auto& [_, c] : clients_) list.push_back(c);
        return list;
    }

    void broadcast(const std::string& line) {
        // Copy the list first so a slow client never blocks joins/leaves.
        for (auto& c : snapshot()) sendTo(*c, line);
    }

    std::shared_ptr<Client> findClient(const std::string& name) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = clients_.find(lower(name));
        return it == clients_.end() ? nullptr : it->second;
    }

    // Handshake: the client sends its name; we answer "OK <name>" or "ERR <reason>"
    // and let it try again until it picks a valid, unused name.
    std::shared_ptr<Client> registerClient(int fd, const std::string& address, LineReader& reader) {
        setTimeout(fd, SO_RCVTIMEO, HANDSHAKE_TIMEOUT_SEC);

        std::string line;
        for (int attempt = 1; attempt <= MAX_NAME_ATTEMPTS; ++attempt) {
            if (!reader.readLine(line)) {
                sendLine(fd, "ERR Timed out waiting for a name.");
                return nullptr;
            }
            std::string name = trim(sanitize(line));
            std::string error = validateName(name);
            if (error.empty()) {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                if (clients_.count(lower(name))) {
                    error = "Name '" + name + "' is already taken.";
                } else {
                    auto client = std::make_shared<Client>();
                    client->fd = fd;
                    client->name = name;
                    client->address = address;
                    clients_[lower(name)] = client;
                    sendLine(fd, "OK " + name);
                    setTimeout(fd, SO_RCVTIMEO, 0); // no idle limit once chatting
                    return client;
                }
            }
            sendLine(fd, "ERR " + error);
        }
        sendLine(fd, "ERR Too many attempts, disconnecting.");
        return nullptr;
    }

    void handleClient(int fd, std::string ip, std::string address) {
        LineReader reader(fd);
        auto client = registerClient(fd, address, reader);
        if (!client) {
            ::close(fd);
            releaseSlot(ip);
            return;
        }

        log(client->name + " joined from " + address);
        sendTo(*client, "*** Welcome to ChatServer, " + client->name +
                        "! Type /help for commands. ***");
        broadcast("*** " + client->name + " joined the chat ***");

        RateLimiter limiter;
        int strikes = 0;
        bool kicked = false;

        std::string line;
        while (reader.readLine(line)) {
            line = trim(sanitize(line));
            if (line.empty()) continue;
            if (line.size() > MAX_MSG_LEN) line.resize(MAX_MSG_LEN);

            if (!limiter.allow()) {
                if (++strikes >= MAX_RATE_STRIKES) {
                    sendTo(*client, "*** Kicked for spamming. ***");
                    kicked = true;
                    break;
                }
                sendTo(*client, "*** Slow down! That message was not sent. ***");
                continue;
            }

            if (line[0] == '/') {
                if (!handleCommand(*client, line)) break;
            } else {
                log(client->name + ": " + line);
                broadcast("[" + timestamp() + "] " + client->name + ": " + line);
            }
        }

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.erase(lower(client->name));
        }
        {
            std::lock_guard<std::mutex> lock(client->sendMutex);
            client->closed = true;
            ::close(fd);
        }
        releaseSlot(ip);

        log(client->name + (kicked ? " was kicked for spamming" : " left"));
        broadcast("*** " + client->name + (kicked ? " was kicked for spamming ***"
                                                   : " left the chat ***"));
    }

    // Returns false if the client asked to disconnect.
    bool handleCommand(Client& client, const std::string& line) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        cmd = lower(cmd);

        if (cmd == "/quit" || cmd == "/exit") {
            sendTo(client, "*** Goodbye! ***");
            return false;
        }
        if (cmd == "/help") {
            sendTo(client, "*** Commands ***");
            sendTo(client, "  /users              list online users");
            sendTo(client, "  /msg <name> <text>  send a private message");
            sendTo(client, "  /me <action>        send an action, e.g. /me waves");
            sendTo(client, "  /quit               leave the chat");
            return true;
        }
        if (cmd == "/users" || cmd == "/list") {
            auto list = snapshot();
            std::string names;
            for (auto& c : list) names += (names.empty() ? "" : ", ") + c->name;
            sendTo(client, "*** Online (" + std::to_string(list.size()) + "): " + names + " ***");
            return true;
        }
        if (cmd == "/msg" || cmd == "/w") {
            std::string target, text;
            in >> target;
            std::getline(in, text);
            text = trim(text);
            if (target.empty() || text.empty()) {
                sendTo(client, "*** Usage: /msg <name> <text> ***");
                return true;
            }
            auto other = findClient(target);
            if (!other) {
                sendTo(client, "*** No user named '" + target + "' is online. ***");
                return true;
            }
            sendTo(*other, "[" + timestamp() + "] (whisper from " + client.name + "): " + text);
            sendTo(client, "[" + timestamp() + "] (whisper to " + other->name + "): " + text);
            return true;
        }
        if (cmd == "/me") {
            std::string action;
            std::getline(in, action);
            action = trim(action);
            if (!action.empty()) broadcast("* " + client.name + " " + action);
            return true;
        }

        sendTo(client, "*** Unknown command " + cmd + ". Type /help. ***");
        return true;
    }
};

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN); // writing to a closed socket must not kill the server

    int port = DEFAULT_PORT;
    if (argc > 1) port = std::atoi(argv[1]);
    else if (const char* env = std::getenv("CHAT_PORT")) port = std::atoi(env);
    if (port <= 0 || port > 65535) {
        std::cerr << "Usage: " << argv[0] << " [port]" << std::endl;
        return 1;
    }

    ChatServer server(port);
    if (!server.start()) return 1;
    server.run();
}
