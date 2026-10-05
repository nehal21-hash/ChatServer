// ChatServer - server
// Accepts many clients, each with a unique name, and relays messages
// to everyone in a single global chat room.

#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cctype>
#include <csignal>
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
};

class ChatServer {
public:
    explicit ChatServer(int port) : port_(port) {}

    bool start() {
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
        if (::listen(listenFd_, 16) < 0) { perror("listen"); return false; }

        log("Server listening on port " + std::to_string(port_));
        return true;
    }

    void run() {
        while (true) {
            sockaddr_in clientAddr{};
            socklen_t len = sizeof(clientAddr);
            int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &len);
            if (fd < 0) { perror("accept"); continue; }

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
            std::string address = std::string(ip) + ":" + std::to_string(ntohs(clientAddr.sin_port));

            std::thread(&ChatServer::handleClient, this, fd, address).detach();
        }
    }

private:
    int port_;
    int listenFd_ = -1;
    std::mutex clientsMutex_;
    std::map<std::string, std::shared_ptr<Client>> clients_; // key: lowercase name

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

    static void sendTo(Client& c, const std::string& line) {
        std::lock_guard<std::mutex> lock(c.sendMutex);
        sendLine(c.fd, line);
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
        std::string line;
        while (reader.readLine(line)) {
            std::string name = trim(line);
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
                    return client;
                }
            }
            sendLine(fd, "ERR " + error);
        }
        return nullptr;
    }

    void handleClient(int fd, std::string address) {
        LineReader reader(fd);
        auto client = registerClient(fd, address, reader);
        if (!client) {
            ::close(fd);
            return;
        }

        log(client->name + " joined from " + address);
        sendTo(*client, "*** Welcome to ChatServer, " + client->name +
                        "! Type /help for commands. ***");
        broadcast("*** " + client->name + " joined the chat ***");

        std::string line;
        while (reader.readLine(line)) {
            line = trim(line);
            if (line.empty()) continue;
            if (line.size() > MAX_MSG_LEN) line.resize(MAX_MSG_LEN);

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
        ::close(fd);
        log(client->name + " left");
        broadcast("*** " + client->name + " left the chat ***");
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
    if (port <= 0 || port > 65535) {
        std::cerr << "Usage: " << argv[0] << " [port]" << std::endl;
        return 1;
    }

    ChatServer server(port);
    if (!server.start()) return 1;
    server.run();
}
