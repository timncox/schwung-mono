#!/usr/bin/env bash
# Cross-compile Mono for Ableton Move (aarch64 Linux) and package clean
# GNU-tar archives. No hardware is touched by this script.
set -euo pipefail
cd "$(dirname "$0")/.."

# Reuse the local toolchain image shared by Smack and Mark. It contains the
# same Debian bookworm aarch64 compiler and avoids maintaining another image.
IMAGE=smack-build
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Wpedantic -Iinclude -Isrc"

if ! docker image inspect "$IMAGE" &>/dev/null; then
    docker build -t "$IMAGE" - <<'EOF'
FROM debian:bookworm
RUN apt-get update && apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu file && rm -rf /var/lib/apt/lists/*
EOF
fi

rm -rf build/modules/sound_generators/mono-voice build/modules/overtake/mono
mkdir -p build/modules/sound_generators/mono-voice build/modules/overtake/mono

cp modules/sound_generators/mono-voice/module.json build/modules/sound_generators/mono-voice/
cp src/ui_chain.js build/modules/sound_generators/mono-voice/
cp src/help_voice.json build/modules/sound_generators/mono-voice/help.json

cp modules/overtake/mono/module.json build/modules/overtake/mono/
cp src/ui_overtake.js build/modules/overtake/mono/ui.js
cp src/help_mono.json build/modules/overtake/mono/help.json

docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c "
    set -e
    aarch64-linux-gnu-gcc $CFLAGS src/mono_core.c src/mono_voice.c \
        -o build/modules/sound_generators/mono-voice/dsp.so -lm
    aarch64-linux-gnu-gcc $CFLAGS src/mono_core.c src/mono_overtake.c \
        -o build/modules/overtake/mono/dsp.so -lm
    file build/modules/sound_generators/mono-voice/dsp.so build/modules/overtake/mono/dsp.so
    tar --owner=0 --group=0 -czf build/mono-voice-module.tar.gz -C build/modules/sound_generators mono-voice
    tar --owner=0 --group=0 -czf build/mono-module.tar.gz -C build/modules/overtake mono
    echo 'tarball contents:'
    tar -tzf build/mono-voice-module.tar.gz
    tar -tzf build/mono-module.tar.gz
"

echo "Built: build/mono-voice-module.tar.gz, build/mono-module.tar.gz"
