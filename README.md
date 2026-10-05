# ChatServer

A multi-client terminal chat in C++17. Every client picks a unique name and talks in one global chat room, like a Discord channel.

## Build

```sh
make            # builds ./server and ./client
make clean
```

## Run

```sh
./server [port]                 # default port 5555 (or $CHAT_PORT)
./client [host] [port]          # default 127.0.0.1 5555 (or $CHAT_HOST / $CHAT_PORT)
./client host:port              # e.g. ./client bore.pub:5817
```

Open several terminals and run `./client` in each one. To chat across machines on the same network, pass the server's IP address: `./client 192.168.1.20`.

The server listens on both IPv4 and IPv6, so `./client ::1` or a global IPv6 address works as well. To avoid typing the address every time, set it in your shell profile:

```sh
export CHAT_HOST=chat.example.com   # IP address or hostname of the server
```

No phone app? Any raw TCP tool works, because the protocol is plain text: `nc <host> 5555`, then type your name.

## Sharing it on the internet

### Quick start: `make share`

```sh
brew install bore-cli   # once (Linux/Windows: cargo install bore-cli)
make share
```

```
  ChatServer is online at  bore.pub:5817

  Share one of these with your friends:
    ChatServer client:            ./client bore.pub:5817
    Mac / Linux / iSH / Termux:   nc bore.pub 5817
    Windows (with Ncat):          ncat bore.pub 5817
```

This starts the server and opens a free public tunnel through [bore](https://github.com/ekzhang/bore). It doesn't need an account or any router settings, and it works **behind CGNAT**. Your friends connect to `bore.pub`, which relays their traffic to your machine, so your home IP is never shared. Press Ctrl+C to stop.

- The public port changes on every run. Use `BORE_PORT=6094 make share` to ask for the same one again. You'll get it if it's free.
- If a server is already running on the port, the script uses that server instead of starting a new one.
- Anyone who finds the address can join, and bore.pub ports do get scanned. The protections below keep the server healthy.

### Why port forwarding may not work (CGNAT)

Open your router's status page and compare its **Internet/WAN IP** with your public IP (`curl -4 ifconfig.me`). If the WAN IP starts with `10.` or `100.64–127.`, or simply doesn't match, your provider puts many customers behind one shared address (CGNAT). Connections from outside never reach your router, so its port forwarding rules have no effect. Use `make share`, or ask your provider for a public IP.

### Other options

1. **Port forwarding:** only works if your router's WAN IP is your public IP. Forward TCP 5555 to your computer's local IP, and reserve that IP for it in the router's DHCP settings.
2. **Cloud server:** run `./server` on a VPS (for example the Oracle Cloud free tier) and open TCP 5555 in its firewall. It stays online when your computer is off.
3. **Tailscale:** private and encrypted, for friends who install [Tailscale](https://tailscale.com) and join your network.

Don't commit your home IP to a public repo. Pass it as an argument or put it in `CHAT_HOST`.

### Built-in protections

| Protection | Limit |
|---|---|
| Total connections | 100 |
| Connections per IP address | 5 (not applied to localhost, since tunnel users all appear to come from there) |
| Time to pick a name | 60 s, at most 5 attempts |
| Message rate | bursts of 8, then 1 per second; spammers are kicked after 15 dropped messages |
| Slow or stuck clients | dropped if they stop reading for 5 s |
| Terminal escape codes | stripped from names and messages |
| Message length | 1024 characters |

The limits are constants in `src/common.h`. Chat is **not encrypted**, so don't send passwords or personal information over it.

## Commands

| Command              | Description                     |
|----------------------|---------------------------------|
| `/help`              | Show commands                   |
| `/users`             | List online users               |
| `/msg <name> <text>` | Private message (alias `/w`)    |
| `/me <action>`       | Action message, e.g. `/me waves`|
| `/quit`              | Leave the chat                  |

## How it works

- **Protocol:** plain text over TCP, one message per line (`\n`).
- **Handshake:** the client sends its name, and the server replies `OK <name>` or `ERR <reason>`. The client keeps asking until a name is accepted. Names are 1–20 characters long (letters, digits, `_` and `-`) and are unique regardless of case.
- **Server:** runs one thread per client. A mutex guards the shared map of clients, and each socket has its own send lock so messages never get mixed together.
- **Client:** a background thread prints incoming messages while the main thread reads your keyboard input.

```
src/common.h   shared helpers and limits (sendLine, LineReader framing, sanitize)
src/server.cpp server
src/client.cpp client
share.sh       starts the server and a public bore tunnel (make share)
```
