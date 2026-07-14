#!/usr/bin/env bash
# Deploy already-built Mono artifacts to a Move. This writes to hardware;
# run only with explicit authorization.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-}"
DEST=/data/UserData/schwung/modules
SSH_ARGS=(-o BatchMode=yes -o ConnectTimeout=8)

if [[ -n "${MOVE_SSH_KEY:-}" ]]; then
    SSH_ARGS+=(-i "$MOVE_SSH_KEY")
elif [[ -f "$HOME/.ssh/move_key" ]]; then
    SSH_ARGS+=(-i "$HOME/.ssh/move_key")
fi

if [[ -z "$HOST" ]]; then
    for candidate in ableton@move.local ableton@move-2.local; do
        if ssh "${SSH_ARGS[@]}" "$candidate" true 2>/dev/null; then
            HOST="$candidate"
            break
        fi
    done
fi

[[ -n "$HOST" ]] || {
    echo "Move not found. Set MOVE_HOST=ableton@<hostname-or-ip>."
    exit 1
}

[ -f build/modules/sound_generators/mono-voice/dsp.so ] || { echo "run scripts/build.sh first"; exit 1; }
[ -f build/modules/overtake/mono/dsp.so ] || { echo "run scripts/build.sh first"; exit 1; }

ssh "${SSH_ARGS[@]}" "$HOST" "mkdir -p $DEST/sound_generators $DEST/overtake"
scp "${SSH_ARGS[@]}" -r build/modules/sound_generators/mono-voice "$HOST:$DEST/sound_generators/"
scp "${SSH_ARGS[@]}" -r build/modules/overtake/mono "$HOST:$DEST/overtake/"
echo "Deployed Mono to $HOST:$DEST — rescan modules or restart Move."
