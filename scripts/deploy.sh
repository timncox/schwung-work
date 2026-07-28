#!/usr/bin/env bash
# Deploy already-built Work, Work In and Overwork artifacts to a Move.
#
# THIS WRITES TO HARDWARE. Run only with explicit authorization.
# Build first with scripts/build.sh — this script never compiles.
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

[ -f build/modules/audio_fx/work/work.so ]              || { echo "run scripts/build.sh first"; exit 1; }
[ -f build/modules/sound_generators/work-in/dsp.so ]    || { echo "run scripts/build.sh first"; exit 1; }
[ -f build/modules/overtake/overwork/dsp.so ]           || { echo "run scripts/build.sh first"; exit 1; }

ssh "${SSH_ARGS[@]}" "$HOST" "mkdir -p $DEST/audio_fx $DEST/sound_generators $DEST/overtake"
scp "${SSH_ARGS[@]}" -r build/modules/audio_fx/work            "$HOST:$DEST/audio_fx/"
scp "${SSH_ARGS[@]}" -r build/modules/sound_generators/work-in "$HOST:$DEST/sound_generators/"
scp "${SSH_ARGS[@]}" -r build/modules/overtake/overwork        "$HOST:$DEST/overtake/"

echo "Deployed Work + Work In + Overwork to $HOST:$DEST"
echo
echo "On the device:"
echo "  Work      appears as an audio effect in the Signal Chain and Master FX."
echo "  Work In   appears as a sound generator you can load into a chain slot."
echo "  Overwork  appears in the overtake menu."
echo
echo "If either was already running, FULLY exit it (Shift + Volume + jog click"
echo "from inside) and relaunch — Back only suspends, and a suspended session"
echo "resumes the OLD code."
