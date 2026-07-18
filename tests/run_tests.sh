#!/usr/bin/env bash
#
# Repair-correctness matrix for the chkdsk core, run on the host build.
#
# For every FS type x corruption scenario x repair mode:
#   1. generate a fresh volume and inject the corruption
#   2. read-only run  -> must detect (exit 1)
#   3. repair run(s)  -> /F or /F /C (up to MAX_PASSES)
#   4. read-only run  -> must be clean (exit 0)
#   5. mkimg.py verify -> filesystem invariants must hold
#
# Usage: run_tests.sh [fat16|fat12|fat32]...   (default: all three)
#
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
HOST="$script_dir/host/chkdsk_host"
MKIMG="$script_dir/mkimg.py"
WORK="${TMPDIR:-/tmp}/chkdsk_tests.$$"
# Deeply nested damage (e.g. chkmess: cross-linked CHK entries whose
# repair exposes an excess chain on the next walk) legitimately needs
# one extra pass; each pass must strictly shrink the damage.
MAX_PASSES=3

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

fstypes=("$@")
[ ${#fstypes[@]} -eq 0 ] && fstypes=(fat16 fat12 fat32)

scenarios=(orphan selfloop cycle2 cycle3tail garb desync broken excess cross userdisk chkmess badname badname_lfn)
modes=("F" "FC")

pass=0
fail=0
declare -a failures

log="$WORK/last.log"

run_case() {
    local fs="$1" sc="$2" mode="$3"
    local img="$WORK/$fs-$sc-$mode.img"
    local tag="[$fs/$sc/${mode}]"

    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1 || { echo "$tag GEN FAIL"; return 1; }
    python3 "$MKIMG" corrupt "$img" "$sc" >>"$log" 2>&1 || { echo "$tag CORRUPT FAIL"; return 1; }

    "$HOST" "$img" >>"$log" 2>&1
    local rc=$?
    if [ $rc -ne 1 ]; then
        echo "$tag DETECT FAIL (ro exit=$rc, want 1)"
        return 1
    fi

    local args=("/F")
    [ "$mode" = "FC" ] && args=("/F" "/C")
    local i
    for i in $(seq 1 $MAX_PASSES); do
        "$HOST" "$img" "${args[@]}" >>"$log" 2>&1
        rc=$?
        if [ $rc -eq 0 ]; then break; fi
        if [ $rc -ge 2 ]; then
            echo "$tag REPAIR ERROR (pass $i exit=$rc)"
            return 1
        fi
    done

    "$HOST" "$img" >>"$log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "$tag NOT CLEAN after $MAX_PASSES repair pass(es) (ro exit=$rc)"
        return 1
    fi

    local vargs=()
    # free-mode discards orphan data by design; converted CHK files keep it
    python3 "$MKIMG" verify "$img" "${vargs[@]}" >>"$log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "$tag VERIFY FAIL"
        tail -12 "$log" | sed 's/^/    /'
        return 1
    fi
    return 0
}

# clean-volume false-positive guard, under both plain and WIN3-shared I/O
# (the WIN3-aliasing bug only surfaced a false "invalid=55" in share mode)
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-clean.img"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    for share in 0 1; do
        if [ "$share" = 1 ]; then export CHKDSK_WIN3_SHARE=1; sfx="/share"; else unset CHKDSK_WIN3_SHARE; sfx=""; fi
        "$HOST" "$img" >>"$log" 2>&1
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "[$fs/clean$sfx] FALSE POSITIVE (exit=$rc)"
            tail -20 "$log" | sed 's/^/    /'
            fail=$((fail+1)); failures+=("$fs/clean$sfx")
        else
            echo "[$fs/clean$sfx] ok"
            pass=$((pass+1))
        fi
    done
    unset CHKDSK_WIN3_SHARE
    python3 "$MKIMG" verify "$img" >>"$log" 2>&1 || {
        echo "[$fs/clean] generator image fails own verify"; fail=$((fail+1)); failures+=("$fs/clean-verify"); }
done

# name-collision guard: badname_collide adds a lowercase twin of FILE2
# ("file2   TXT") whose sanitized (uppercased) name would collide with
# the real FILE2 entry. Unlike every other scenario, the expected
# post-/F state is NOT clean -- the rename must be skipped, so this
# runs its own assertions instead of going through run_case/run_variant.
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-badname_collide.img"
    tag="[$fs/badname_collide]"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    python3 "$MKIMG" corrupt "$img" badname_collide >>"$log" 2>&1

    "$HOST" "$img" >>"$log" 2>&1
    rc=$?
    if [ $rc -ne 1 ]; then
        echo "$tag DETECT FAIL (ro exit=$rc, want 1)"; fail=$((fail+1)); failures+=("$fs/badname_collide/detect"); continue
    fi

    "$HOST" "$img" /F >>"$log" 2>&1
    "$HOST" "$img" >>"$log" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "$tag FAIL: collision was fixed anyway (should stay flagged)"
        fail=$((fail+1)); failures+=("$fs/badname_collide/still-flagged")
    elif ! python3 "$MKIMG" hasname "$img" "file2   TXT" >>"$log" 2>&1; then
        echo "$tag FAIL: lowercase twin was renamed/deleted"
        fail=$((fail+1)); failures+=("$fs/badname_collide/twin-intact")
    elif ! python3 "$MKIMG" hasname "$img" "FILE2   TXT" >>"$log" 2>&1; then
        echo "$tag FAIL: original FILE2 entry damaged"
        fail=$((fail+1)); failures+=("$fs/badname_collide/original-intact")
    else
        echo "$tag ok (skipped, both names intact)"
        pass=$((pass+1))
    fi
done

# Each case runs under three I/O models, so a repair must be correct
# against all of them:
#   plain  -- separate host buffers (fast, catches pure logic bugs)
#   share  -- CHKDSK_WIN3_SHARE=1: sectbuf/batch/bitmap share one 16 KB
#             window with page-swap semantics, exactly like Sprinter WIN3.
#             Catches aliasing bugs invisible with separate buffers.
#   stale  -- CHKDSK_STALE_BATCH=1: batch reads served from an open-time
#             snapshot (in-run writes invisible), modelling incoherent
#             multi-sector reads.
run_variant() {
    local sfx="$1"
    for fs in "${fstypes[@]}"; do
        for sc in "${scenarios[@]}"; do
            for mode in "${modes[@]}"; do
                if run_case "$fs" "$sc" "$mode"; then
                    echo "[$fs/$sc/$mode$sfx] ok"
                    pass=$((pass+1))
                else
                    fail=$((fail+1))
                    failures+=("$fs/$sc/$mode$sfx")
                fi
            done
        done
    done
}

unset CHKDSK_WIN3_SHARE CHKDSK_STALE_BATCH
run_variant ""
export CHKDSK_WIN3_SHARE=1; run_variant "/share"; unset CHKDSK_WIN3_SHARE
export CHKDSK_STALE_BATCH=1; run_variant "/stale"; unset CHKDSK_STALE_BATCH

echo
echo "==== $pass passed, $fail failed ===="
if [ $fail -gt 0 ]; then
    printf '  FAILED: %s\n' "${failures[@]}"
    exit 1
fi
exit 0
