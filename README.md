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
```

Open several terminals and run `./client` in each one. To chat across machines on the same network, pass the server's IP address: `./client 192.168.1.20`.

The server listens on both IPv4 and IPv6, so `./client ::1` or a global IPv6 address works as well. To avoid typing the address every time, set it in your shell profile:

```sh
export CHAT_HOST=chat.example.com   # IP address or hostname of the server
```

No phone app? Any raw TCP tool works, because the protocol is plain text: `nc <host> 5555`, then type your name.

## Hosting it on the internet

The code is ready for this. Pick one of these options:

1. **Cloud server (recommended):** run `./server` on a VPS, for example the Oracle Cloud free tier, and open TCP port 5555 in its firewall. You get a fixed public IP, and your home network is never exposed.
2. **Tailscale:** you and your friends install [Tailscale](https://tailscale.com), and they connect to your Tailscale IP. It's encrypted, needs no router setup and isn't visible to strangers.
3. **Home router port forwarding:** forward TCP 5555 to your computer's local IP, and reserve that IP for it in the router's DHCP settings. Check first that your router's WAN IP matches your public IP. If it doesn't, your provider uses CGNAT, and port forwarding won't work.

Don't commit your home IP to a public repo. Pass it as an argument or put it in `CHAT_HOST`.

### Built-in protections

| Protection | Limit |
|---|---|
| Total connections | 100 |
| Connections per IP address | 5 |
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
```
