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
cp src/sample_io.mjs                 build/modules/audio_fx/work/
cp src/help_work.json                build/modules/audio_fx/work/help.json

cp modules/sound_generators/work-in/module.json build/modules/sound_generators/work-in/
cp src/ui_chain.js                              build/modules/sound_generators/work-in/
cp src/sample_io.mjs                            build/modules/sound_generators/work-in/
cp src/help_work.json                           build/modules/sound_generators/work-in/help.json

cp modules/overtake/overwork/module.json build/modules/overtake/overwork/
mkdir -p build/modules/overtake/overwork-mix
cp modules/overtake/overwork-mix/module.json build/modules/overtake/overwork-mix/
cp src/ui_overtake.js                    build/modules/overtake/overwork/ui.js
cp src/ui_overtake.js                    build/modules/overtake/overwork-mix/ui.js
cp src/sample_io.mjs                     build/modules/overtake/overwork/
cp src/sample_io.mjs                     build/modules/overtake/overwork-mix/
cp src/help_overwork.json                build/modules/overtake/overwork/help.json
cp src/help_overwork_mix.json            build/modules/overtake/overwork-mix/help.json

# Browser editor. schwung-manager auto-discovers web_ui.html per module and
# serves it in a sandboxed iframe. ONE source file for both, because the page
# sniffs its own parameter prefix at runtime ("overtake_dsp:" as the tool,
# "synth:" in a slot) rather than being built twice.
#
# The audio_fx build does NOT get one yet, and it is NOT because the host
# refuses: upstream v1.2.0 serves web_ui.html for any chain component, audio FX
# included (docs/MODULES.md, "Remote UI Custom HTML"). The page itself is the
# gap — it sniffs its prefix from the first update burst and only recognises
# "overtake_dsp:" and "synth:", so in an fx1/fx2 section it would never latch a
# prefix and would sit inert. Teach it those two prefixes and this copy can be
# added. Serving ui_hierarchy is NOT the alternative: see CLAUDE.md, "The chain
# slot has no Remote UI" -- it takes the on-device editor away.
cp src/web_ui.html build/modules/sound_generators/work-in/web_ui.html
cp src/web_ui.html build/modules/overtake/overwork/web_ui.html
cp src/web_ui.html build/modules/overtake/overwork-mix/web_ui.html

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
    aarch64-linux-gnu-gcc $CFLAGS src/work_core.c src/work_overtake_fx.c \
        -o build/modules/overtake/overwork-mix/dsp.so -lm
    file build/modules/audio_fx/work/work.so \
         build/modules/sound_generators/work-in/dsp.so \
         build/modules/overtake/overwork/dsp.so
    tar --owner=0 --group=0 -czf build/work-module.tar.gz -C build/modules/audio_fx work
    tar --owner=0 --group=0 -czf build/work-in-module.tar.gz -C build/modules/sound_generators work-in
    tar --owner=0 --group=0 -czf build/overwork-module.tar.gz -C build/modules/overtake overwork
    tar --owner=0 --group=0 -czf build/overwork-mix-module.tar.gz -C build/modules/overtake overwork-mix
    echo 'tarball contents:'
    tar -tzf build/work-module.tar.gz
    tar -tzf build/work-in-module.tar.gz
    tar -tzf build/overwork-module.tar.gz
"

echo "Built: build/work-module.tar.gz, build/work-in-module.tar.gz, build/overwork-module.tar.gz, build/overwork-mix-module.tar.gz"
