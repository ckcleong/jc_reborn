#!/bin/bash
#
# Gate checker for baseline suites produced by tools/run_baseline.sh.
#
# Usage: tools/compare_baseline.sh <ref-dir> <test-dir>
#   exit 0 = gate passed
#
# Gate protocol (see docs/TESTING.md):
# - short legs (bench, islandads, fadetest, mjfish, gjvis5, gjvis6) end
#   naturally at fixed frame counts -> gate is full-file byte identity.
# - story is cut by a wall-clock timeout while frame delivery is paced by
#   SDL_GetTicks, so the exact number of frames inside the window jitters
#   (measured +-12 frames over 300 s). Its content is deterministic
#   (srand(42) + JC_TESTTIME pins), so its gate is byte identity over the
#   COMMON PREFIX plus a bounded count delta.

set -u
ref="${1:?Usage: compare_baseline.sh <ref-dir> <test-dir>}"
tst="${2:?Usage: compare_baseline.sh <ref-dir> <test-dir>}"

fpsz=153600                # bytes/frame (RGB565 320x240)
storyMaxDelta=200          # sanity band on where the wall-clock cut lands (see note below)

# The story leg is cut by a wall-clock timeout, so its frame count is a
# machine-time property, not a content property: DVFS/governor drift
# across hours can shift the count versus a reference captured under
# different machine conditions, while runs within one machine state stay
# within about +-12 frames. Only the COMMON PREFIX is a content
# invariant; any content regression fails there. |delta| > 200 means the
# cut landed implausibly early/late (machine pathology) -> investigate,
# but it is not content evidence.

printf '%-10s %9s %-6s %s\n' leg frames gate note
rc=0

for leg in bench islandads fadetest mjfish gjvis5 gjvis6 story; do
    rf="$ref/$leg/frames.bin"; tf="$tst/$leg/frames.bin"

    if [ ! -f "$rf" ] || [ ! -f "$tf" ]; then
        printf '%-10s %9s FAIL  %s\n' "$leg" "-" "missing frames.bin"
        rc=1; continue
    fi

    r=$(( $(stat -c %s "$rf") / fpsz ))
    t=$(( $(stat -c %s "$tf") / fpsz ))

    if [ "$leg" = story ]; then
        n=$(( r < t ? r : t ))                                  # common prefix
        if cmp -s -n "$((n * fpsz))" "$rf" "$tf"; then
            delta=$(( t - r ))
            gate=PREFIX-PASS; note="first $n f identical, count delta $delta"
            if [ "$delta" -gt "$storyMaxDelta" ] || [ "$delta" -lt "-$storyMaxDelta" ]; then
                gate=PREFIX-JITTER; rc=1
            fi
        else
            byte=$(cmp "$rf" "$tf" 2>&1 | sed -n 's/^.*differ: byte \([0-9]*\).*/\1/p')
            gate=FAIL
            [ "$byte" -gt 0 ] 2>/dev/null && note="first diff at frame $(( byte / fpsz ))" || note="undetermined diff"
            rc=1
        fi
        printf '%-10s %7d/%d %-6s %s\n' "$leg" "$r" "$t" "$gate" "$note"
    else
        if [ "$r" -eq "$t" ] && cmp -s "$rf" "$tf"; then
            printf '%-10s %9d PASS\n' "$leg" "$r"
        else
            printf '%-10s %7d/%d FAIL  %s\n' "$leg" "$r" "$t" \
                "$([ "$r" -eq "$t" ] && echo "content differs (same count)" || echo "frame count differs")"
            rc=1
        fi
    fi
done

exit $rc
