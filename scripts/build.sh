#!/usr/bin/env bash
# Cross-compile Work, Work In and Overwork for Ableton Move (aarch64 Linux) and package
# clean GNU-tar archives. No hardware is touched by this script.
#
# All three builds compile from the SAME src/work_core.c — one engine, three
# wrappers. Keep them tagged together.
set -euo pipefail
cd "$(dirname "$0")/.."

# Reuse the toolchain image shared by Smack, Mark and Mono — same Debian
# bookworm aarch64 compiler, one image to maintain.
IMAGE=smack-build
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Wpedantic -Iinclude -Isrc"

if ! docker image inspect "$IMAGE" &>/dev/null; then
    docker build -t "$IMAGE" - <<'EOF'
FROM debian:bookworm
RUN apt-get update && apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu file && rm -rf /var/lib/apt/lists/*
EOF
fi

rm -rf build/modules
mkdir -p build/modules/audio_fx/work build/modules/sound_generators/work-in build/modules/overtake/overwork

cp modules/audio_fx/work/module.json build/modules/audio_fx/work/
cp src/ui_chain.js                   build/modules/audio_fx/work/
cp src/help_work.json                build/modules/audio_fx/work/help.json

cp modules/sound_generators/work-in/module.json build/modules/sound_generators/work-in/
cp src/ui_chain.js                              build/modules/sound_generators/work-in/
cp src/help_work.json                           build/modules/sound_generators/work-in/help.json

cp modules/overtake/overwork/module.json build/modules/overtake/overwork/
cp src/ui_overtake.js                    build/modules/overtake/overwork/ui.js
cp src/help_overwork.json                build/modules/overtake/overwork/help.json

# The chain host loads a slot's audio FX as modules/audio_fx/<id>/<id>.so and
# never reads module.json's "dsp" field, so the FX build MUST be work.so.
# Overtake modules are loaded via their module.json, hence plain dsp.so.
#
# tar runs INSIDE the container on purpose: macOS tar writes ._* AppleDouble
# entries that are invisible on the Mac and break the installer on the device.
docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c "
    set -e
    aarch64-linux-gnu-gcc $CFLAGS src/work_core.c src/work_fx.c \
        -o build/modules/audio_fx/work/work.so -lm
    aarch64-linux-gnu-gcc $CFLAGS src/work_core.c src/work_gen.c \
        -o build/modules/sound_generators/work-in/dsp.so -lm
    aarch64-linux-gnu-gcc $CFLAGS src/work_core.c src/work_overtake.c \
        -o build/modules/overtake/overwork/dsp.so -lm
    file build/modules/audio_fx/work/work.so \
         build/modules/sound_generators/work-in/dsp.so \
         build/modules/overtake/overwork/dsp.so
    tar --owner=0 --group=0 -czf build/work-module.tar.gz -C build/modules/audio_fx work
    tar --owner=0 --group=0 -czf build/work-in-module.tar.gz -C build/modules/sound_generators work-in
    tar --owner=0 --group=0 -czf build/overwork-module.tar.gz -C build/modules/overtake overwork
    echo 'tarball contents:'
    tar -tzf build/work-module.tar.gz
    tar -tzf build/work-in-module.tar.gz
    tar -tzf build/overwork-module.tar.gz
"

echo "Built: build/work-module.tar.gz, build/work-in-module.tar.gz, build/overwork-module.tar.gz"
