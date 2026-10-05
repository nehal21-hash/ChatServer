# ChatServer

A multi-client terminal chat in C++17. Every client picks a unique name and talks in one global chat room, like a Discord channel.

## Build

```sh
make            # builds ./server and ./client
make clean
```

## Run

```sh
./server [port]                 # default port 5555
./client [host] [port]          # default 127.0.0.1 5555
```

Open several terminals and run `./client` in each one. To chat across machines on the same network, pass the server's IP address: `./client 192.168.1.20`.

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
src/common.h   shared helpers (sendLine, LineReader framing)
src/server.cpp server
src/client.cpp client
```
