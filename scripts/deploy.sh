#!/usr/bin/env bash
# Deploy already-built Mono artifacts to a Move. This writes to hardware;
# run only with explicit authorization.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-ableton@move.local}"
DEST=/data/UserData/schwung/modules

[ -f build/modules/sound_generators/mono-voice/dsp.so ] || { echo "run scripts/build.sh first"; exit 1; }
[ -f build/modules/overtake/mono/dsp.so ] || { echo "run scripts/build.sh first"; exit 1; }

ssh "$HOST" "mkdir -p $DEST/sound_generators $DEST/overtake"
scp -r build/modules/sound_generators/mono-voice "$HOST:$DEST/sound_generators/"
scp -r build/modules/overtake/mono "$HOST:$DEST/overtake/"
echo "Deployed Mono to $HOST:$DEST — rescan modules or restart Move."
