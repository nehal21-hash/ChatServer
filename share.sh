#!/usr/bin/env bash
# Puts ChatServer on the internet, even behind CGNAT or without router access.
# It starts the server (unless one is already running on the port) and opens a
# free public TCP tunnel with bore (https://github.com/ekzhang/bore).
#
#   ./share.sh [local-port]        default 5555
#   BORE_PORT=6094 ./share.sh      ask bore.pub for a specific public port

set -euo pipefail
cd "$(dirname "$0")"

PORT="${1:-${CHAT_PORT:-5555}}"
TUNNEL_HOST="bore.pub"

if ! command -v bore >/dev/null 2>&1; then
    echo "bore is not installed. Install it with:"
    echo "  brew install bore-cli        # macOS"
    echo "  cargo install bore-cli       # Linux / Windows (needs Rust)"
    exit 1
fi

[ -x ./server ] || make server

SERVER_PID=""
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "Using the server that is already running on port $PORT."
else
    ./server "$PORT" &
    SERVER_PID=$!
    sleep 0.5
fi

LOG="$(mktemp -t chatserver-bore.XXXXXX)"
BORE_ARGS=(local "$PORT" --to "$TUNNEL_HOST")
[ -n "${BORE_PORT:-}" ] && BORE_ARGS+=(--port "$BORE_PORT")
bore "${BORE_ARGS[@]}" >"$LOG" 2>&1 &
BORE_PID=$!

cleanup() {
    kill "$BORE_PID" 2>/dev/null || true
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$LOG"
    echo
    echo "Tunnel closed."
}
trap cleanup EXIT INT TERM

# Wait for bore to report the public address.
ADDRESS=""
for _ in $(seq 1 30); do
    ADDRESS="$(sed 's/\x1b\[[0-9;]*m//g' "$LOG" | grep -o "$TUNNEL_HOST:[0-9]*" | head -1 || true)"
    [ -n "$ADDRESS" ] && break
    kill -0 "$BORE_PID" 2>/dev/null || break
    sleep 0.5
done

if [ -z "$ADDRESS" ]; then
    echo "Could not open a tunnel. bore said:"
    cat "$LOG"
    exit 1
fi

HOST="${ADDRESS%:*}"
PUBLIC_PORT="${ADDRESS##*:}"

cat <<EOF

  ChatServer is online at  $ADDRESS

  Share one of these with your friends:
    ChatServer client:            ./client $ADDRESS
    Mac / Linux / iSH / Termux:   nc $HOST $PUBLIC_PORT
    Windows (with Ncat):          ncat $HOST $PUBLIC_PORT
  then type a name and press Enter.

  The address changes every time you restart this script.
  Press Ctrl+C to stop sharing.

EOF

wait "$BORE_PID"
