#!/bin/bash
#
# Re-runs the PC frame-equivalence baseline legs (see
# docs/TESTING.md) and records every dump artifact for later A/B
# comparisons.
#
# Usage: tools/run_baseline.sh <target-dir> [binary]
#
# Each leg gets a subdirectory <target>/<leg>/ containing:
#   frames.bin   — composed frames, RGB565 320x240, 153 600 B/frame
#                  (the exact content the ESP32 SPI push will present)
#   stdout.log   — memstats checkpoints + engine reports
#   stderr.log   — harness warnings (mostly empty)
# The script then writes SUMMARY.md with frame counts (frames.bin size
# / 153 600), md5s, and the [MEM] floor/peak lines per leg.
#
# Notes:
# - binary must be a -DMEMTRACK build for the 'memstats' option to work;
#   CFLAGS="-Wall -Wpedantic -std=c99 -O2 -DMEMTRACK"
# - run with the resource files in the CWD (out/ in this tree)
# - the story/island content depends on the wall clock (island day/night
#   via hour, holiday dates, story-day progression), so the runner pins
#   it: JC_TESTTIME=102014 (Oct 20, hour 14; hour%8=6 => day island).
#   Override by exporting JC_TESTTIME yourself; see utils.c and
#   docs/TESTING.md.
# - the story's day progression additionally persists in $HOME/.jc_reborn
#   (config.c, storyUpdateCurrentDay()): storyCurrentDay drives the raft
#   build progress and the scene pool, so without isolation a story leg's
#   content drifts with whatever state the invoking session has
#   accumulated. Every suite therefore runs with a pinned HOME carrying a
#   fixed config: currentDay=5 & date=292 (the pinned JC_TESTTIME
#   day-of-year, 0-based — no bump is recorded during the run and
#   storyCurrentDay is exactly 5).
# - infinite legs (story, island ads) are cut with `timeout`; the frame
#   counts are valid only as exact-prefix references, reruns must use the
#   same timeouts to get comparable cuts.

export JC_TESTTIME="${JC_TESTTIME:-102014}"    # Oct 20, hour 14 (hour%8=6 => day island)

set -u
target="${1:?Usage: run_baseline.sh <target-dir> [binary]}"
bin="${2:-../jc_reborn}"

target="$(readlink -m "$target")"
mkdir -p "$target"

# Pristine story-day state per suite (see notes above)
export HOME="$target/home"
rm -rf "$target/home"
mkdir -p "$target/home"
printf 'currentDay=5\ndate=292\n' > "$target/home/.jc_reborn"

LEG() {  # LEG <name> <timeout-s> [ENV vars] -- <args...>
    name="$1"; secs="$2"; shift 2
    envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    dir="$target/$name"
    mkdir -p "$dir"
    echo "[run] $name (timeout ${secs}s)"
    SDL_VIDEODRIVER=dummy \
    env "${envs[@]}" stdbuf -oL timeout "$secs" "$bin" "$@" \
        > "$dir/stdout.log" 2> "$dir/stderr.log"
    echo $name $? >> "$target/exits.txt"
}

# The legs:
LEG story    300 JC_FRAMEDUMP="$target/story"     -- window memstats nosound
LEG bench    240 JC_FRAMEDUMP="$target/bench"     JC_BENCHFIX=1   -- bench nosound
LEG islandads 120 JC_FRAMEDUMP="$target/islandads"               -- island ads JOHNNY.ADS 2 nosound
LEG fadetest  90 JC_FRAMEDUMP="$target/fadetest"   JC_FADETEST=1  -- ttm GJNAT3.TTM nosound
LEG mjfish   120 JC_FRAMEDUMP="$target/mjfish"                    -- ttm MJFISH.TTM nosound
LEG gjvis5    60 JC_FRAMEDUMP="$target/gjvis5"                    -- ttm GJVIS5.TTM nosound
LEG gjvis6    60 JC_FRAMEDUMP="$target/gjvis6"                    -- ttm GJVIS6.TTM nosound

# Summary
fmt_frames() { local sz; sz=$(stat -c %s "$1" 2>/dev/null || echo 0); [ "$sz" -eq 0 ] && echo "0 (NO frames.bin)" || echo "$((sz / 153600)) (size ${sz})"; }
{
    printf '# Baseline run %s\n\n' "$(date -Iseconds)"
    printf 'binary: %s\nbuild: make CFLAGS="-Wall -Wpedantic -std=c99 -O2 -DMEMTRACK"\nartifacts under %s (gitignored)\n\n| leg | frames | frames.bin md5 (12) | memstats highlights |\n|---|---|---|---|\n' "$bin" "$target"
    for name in story bench islandads fadetest mjfish gjvis5 gjvis6; do
        f="$target/$name/frames.bin"
        md5=$(md5sum "$f" 2>/dev/null | cut -c1-12 || echo none)
        hl=$(grep -E '\[MEM\] (boot|scene)' "$target/$name/stdout.log" 2>/dev/null | tail -4 | tr -d '\n' | cut -c1-160)
        printf '| %s | %s | %s | %s |\n' "$name" "$(fmt_frames "$f")" "${md5:-none}" "$hl"
    done
} > "$target/SUMMARY.md"

echo "[done] $target/SUMMARY.md"
