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

# Build the host binary from the current sources before doing anything
# else. Without this the suite silently exercises whatever binary was
# left in tests/host from a previous session, so a source change could
# "pass" without ever having been compiled.
if ! make -C "$script_dir/host" >/dev/null; then
    echo "FATAL: host build failed"
    make -C "$script_dir/host"
    exit 1
fi

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

fstypes=("$@")
[ ${#fstypes[@]} -eq 0 ] && fstypes=(fat16 fat12 fat32)

scenarios=(orphan selfloop cycle2 cycle3tail garb desync broken excess cross userdisk chkmess badname badname_lfn badname_lfn_cross badname_cyr lfn_badsum lfn_badsum_cross lfn_badord lfn_badordbit lfn_earlynul lfn_badpad lfn_orphan lfn_overlong)
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

    # Exit codes under /F: 0 clean, 2 all-found-fixed, 3 partially fixed
    # -- all three are legitimate outcomes of a repair pass and may
    # still need another pass (e.g. a fixed entry can surface a new
    # finding on re-scan). Only 255 (fatal: scan aborted, OOM, mount
    # failure) is a hard stop. 1 should never come back under /F --
    # dispatch() only returns it in read-only mode.
    local args=("/F" "/Y")
    [ "$mode" = "FC" ] && args=("/F" "/C" "/Y")
    local i
    for i in $(seq 1 $MAX_PASSES); do
        "$HOST" "$img" "${args[@]}" >>"$log" 2>&1
        rc=$?
        if [ $rc -eq 0 ]; then break; fi
        if [ $rc -eq 255 ] || [ $rc -eq 1 ]; then
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

# Cross-sector SFN commit failure: write #1 restamps the earlier LFN
# sector, write #2 (SFN sector) is injected to fail, and the repair must
# roll write #1 back. The old lowercase SFN and its valid old-checksum
# LFN must remain paired; only the original lowercase issue may remain.
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-lfn-rollback.img"
    tag="[$fs/lfn_rollback]"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    python3 "$MKIMG" corrupt "$img" badname_lfn_cross >>"$log" 2>&1
    CHKDSK_FAIL_WRITE_N=2 "$HOST" "$img" /F /Y >>"$log" 2>&1
    rc=$?
    rout="$WORK/$fs-lfn-rollback-ro.out"
    "$HOST" "$img" >"$rout" 2>>"$log"
    ro_rc=$?
    if [ $rc -ne 3 ]; then
        echo "$tag FAIL: injected write failure exit=$rc, want 3"
        fail=$((fail+1)); failures+=("$fs/lfn_rollback/exit")
    elif [ $ro_rc -ne 1 ]; then
        echo "$tag FAIL: rolled-back image exit=$ro_rc, want original lowercase issue"
        fail=$((fail+1)); failures+=("$fs/lfn_rollback/ro")
    elif grep -q 'lfn-broken' "$rout"; then
        echo "$tag FAIL: failed commit left the LFN checksum mismatched"
        fail=$((fail+1)); failures+=("$fs/lfn_rollback/checksum")
    elif ! python3 "$MKIMG" haslfn "$img" "cross~1 txt" >>"$log" 2>&1; then
        echo "$tag FAIL: old LFN/SFN pair was not restored"
        fail=$((fail+1)); failures+=("$fs/lfn_rollback/pair")
    else
        echo "$tag ok (failed commit rolled back to valid old pair)"
        pass=$((pass+1))
    fi
done

# Cross-sector restamp: the long name must survive while both slots get
# the checksum of the folded SFN. A clean follow-up scan alone is not
# sufficient here because deleting the LFN would also look clean.
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-lfnstamp-cross.img"
    tag="[$fs/lfn_restamp_cross]"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    python3 "$MKIMG" corrupt "$img" badname_lfn_cross >>"$log" 2>&1
    out="$WORK/$fs-lfnstamp-cross.out"
    "$HOST" "$img" /F /Y >"$out" 2>>"$log"
    if ! python3 "$MKIMG" haslfn "$img" "CROSS~1 TXT" >>"$log" 2>&1; then
        echo "$tag FAIL: cross-sector LFN was not preserved/restamped"
        fail=$((fail+1)); failures+=("$fs/lfn_restamp_cross/preserve")
    else
        # Uniform across FAT types: FSInfo invalidation is a bookkeeping
        # side-effect of an already-counted repair, so it no longer bumps
        # "applied" on FAT32. applied must never exceed found.
        if ! grep -q "Fixes: found=1 applied=1" "$out"; then
            echo "$tag FAIL: logical fix counters are not 1/1"
            fail=$((fail+1)); failures+=("$fs/lfn_restamp_cross/count")
        else
            echo "$tag ok (cross-sector group preserved, one logical fix)"
            pass=$((pass+1))
        fi
    fi
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

    "$HOST" "$img" /F /Y >>"$log" 2>&1
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

# CP866 lowercase-Cyrillic fold: badname_cyr's corrupted byte (0xA0)
# must fold to uppercase (0x80) under /F, not get replaced with '_'
# (0x5F) like an ordinary forbidden character -- see dirent_sanitize_name.
# Locate the dirent BEFORE corrupting it: find_file_chain matches via
# ascii-decode, which can never match a name already holding a raw
# high byte, so the offset has to be captured while it's still ASCII.
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-badname_cyr.img"
    tag="[$fs/badname_cyr/fold]"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    off=$(python3 "$MKIMG" locate "$img" "README  TXT" 2>>"$log")
    python3 "$MKIMG" corrupt "$img" badname_cyr >>"$log" 2>&1
    "$HOST" "$img" /F /Y >>"$log" 2>&1
    if python3 "$MKIMG" checkbyte "$img" "$((off + 8))" 0x80 >>"$log" 2>&1; then
        echo "$tag ok"
        pass=$((pass+1))
    else
        echo "$tag FAIL: byte not folded to uppercase"
        tail -5 "$log" | sed 's/^/    /'
        fail=$((fail+1)); failures+=("$fs/badname_cyr/fold")
    fi
done

# WARNING-gate decline guard: /F without /Y must show the one-time
# write warning and wait for a keypress via dss_waitkey_ex -- the host
# shim always returns ascii=0 (never 'Y'), so the gate must decline,
# fix_write must never reach disk_write, and dispatch() must report
# exit 1 (found, unfixed) rather than 2/3, exactly as a plain read-only
# run would. Byte-for-byte image comparison proves no write slipped
# through before the decline was noticed.
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-warngate.img"
    tag="[$fs/warngate]"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    python3 "$MKIMG" corrupt "$img" orphan >>"$log" 2>&1
    sum_before=$(cksum "$img")

    "$HOST" "$img" /F >>"$log" 2>&1
    rc=$?
    sum_after=$(cksum "$img")

    if [ "$sum_before" != "$sum_after" ]; then
        echo "$tag FAIL: image modified despite a declined WARNING gate"
        fail=$((fail+1)); failures+=("$fs/warngate/no-write")
    elif [ $rc -ne 1 ]; then
        echo "$tag FAIL: exit=$rc, want 1 (declined -> found-unfixed)"
        fail=$((fail+1)); failures+=("$fs/warngate/exitcode")
    else
        echo "$tag ok (declined, no writes, exit=1)"
        pass=$((pass+1))
    fi
done

# LFN checksum restamp (specs.md's post-MVP item): badname_lfn puts a
# VALID 2-slot LFN group in front of a lowercase SFN. Folding the SFN to
# upper case invalidates the group's checksum, so the name-fix must
# restamp it in place -- the long name must SURVIVE, not be deleted.
# Asserts the slot right before the SFN is still a live LFN slot
# (order byte 0x01, attr 0x0F) carrying the NEW name's checksum.
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-lfnstamp.img"
    tag="[$fs/lfn_restamp]"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    python3 "$MKIMG" corrupt "$img" badname_lfn >>"$log" 2>&1
    "$HOST" "$img" /F /Y >>"$log" 2>&1
    off=$(python3 "$MKIMG" locate "$img" "LOWER~1 TXT" 2>>"$log")
    want=$(python3 "$MKIMG" lfnsum "LOWER~1 TXT" 2>>"$log")
    if [ -z "$off" ]; then
        echo "$tag FAIL: SFN not folded to LOWER~1 TXT"
        fail=$((fail+1)); failures+=("$fs/lfn_restamp/fold")
    elif ! python3 "$MKIMG" checkbyte "$img" "$((off - 32))" 0x01 >>"$log" 2>&1; then
        echo "$tag FAIL: preceding LFN slot deleted or reordered (want order 0x01)"
        fail=$((fail+1)); failures+=("$fs/lfn_restamp/alive")
    elif ! python3 "$MKIMG" checkbyte "$img" "$((off - 32 + 11))" 0x0F >>"$log" 2>&1; then
        echo "$tag FAIL: preceding slot is no longer an LFN slot"
        fail=$((fail+1)); failures+=("$fs/lfn_restamp/attr")
    elif ! python3 "$MKIMG" checkbyte "$img" "$((off - 32 + 13))" "$want" >>"$log" 2>&1; then
        echo "$tag FAIL: LFN checksum not restamped to $want"
        fail=$((fail+1)); failures+=("$fs/lfn_restamp/sum")
    else
        echo "$tag ok (long name preserved, checksum restamped to $want)"
        pass=$((pass+1))
    fi
done

# End-of-run classic space report: cross-check that scan_print_report
# wired the right data by comparing its "allocation units" figures
# against the independently-printed pre-scan geometry and Phase 2 free
# count, and that the byte figures equal units*cluster_size. Runs on a
# clean image so the identity free+files+dirs+bad == total holds exactly.
for fs in "${fstypes[@]}"; do
    img="$WORK/$fs-report.img"
    tag="[$fs/report]"
    rout="$WORK/$fs-report.out"
    python3 "$MKIMG" build "$img" "$fs" >"$log" 2>&1
    "$HOST" "$img" >"$rout" 2>>"$log"

    # Output is CRLF-terminated; strip \r so a value that lands as the
    # last field on its line doesn't carry a trailing carriage return.
    strip() { tr -d '\r'; }
    tot_units=$(grep 'total allocation units'     "$rout" | awk '{print $1}' | strip)
    avail_units=$(grep 'available allocation units' "$rout" | awk '{print $1}' | strip)
    unit_sz=$(grep 'bytes in each allocation unit'  "$rout" | awk '{print $1}' | strip)
    tot_bytes=$(grep 'bytes total disk space'       "$rout" | awk '{print $1}' | strip)
    pre_total=$(grep 'Total clusters:'              "$rout" | awk '{print $3}' | strip)
    free_ph2=$(grep -o 'free=[0-9]*'                "$rout" | head -1 | cut -d= -f2 | strip)
    used_ph2=$(grep -o 'used=[0-9]*'                "$rout" | head -1 | cut -d= -f2 | strip)
    inval_ph2=$(grep -o 'invalid=[0-9]*'            "$rout" | head -1 | cut -d= -f2 | strip)

    # The two accounting identities. Both were silently violated before:
    # Phase 2 printed "used" without the per-chain EOC terminators, and
    # the report omitted the FAT32 root directory's own clusters.
    files_b=$(grep 'bytes in .* user files'         "$rout" | awk '{print $1}' | strip)
    dirs_b=$(grep 'bytes in .* directories'         "$rout" | awk '{print $1}' | strip)
    bad_b=$(grep 'bytes in bad sectors'             "$rout" | awk '{print $1}' | strip)
    avail_b=$(grep 'bytes available on disk'        "$rout" | awk '{print $1}' | strip)
    ph2_sum=$((free_ph2 + used_ph2 + inval_ph2))
    rep_sum=$((files_b + dirs_b + bad_b + avail_b))

    if [ -z "$tot_units" ] || [ -z "$avail_units" ] || [ -z "$unit_sz" ]; then
        echo "$tag FAIL: report block missing or malformed"
        tail -20 "$rout" | sed 's/^/    /'
        fail=$((fail+1)); failures+=("$fs/report/missing")
    elif [ "$tot_units" != "$pre_total" ]; then
        echo "$tag FAIL: total units $tot_units != pre-scan clusters $pre_total"
        fail=$((fail+1)); failures+=("$fs/report/total")
    elif [ "$avail_units" != "$free_ph2" ]; then
        echo "$tag FAIL: available units $avail_units != Phase 2 free $free_ph2"
        fail=$((fail+1)); failures+=("$fs/report/free")
    elif [ "$tot_bytes" != "$((tot_units * unit_sz))" ]; then
        echo "$tag FAIL: total bytes $tot_bytes != units*size $((tot_units * unit_sz))"
        fail=$((fail+1)); failures+=("$fs/report/bytes")
    elif [ "$ph2_sum" != "$tot_units" ]; then
        echo "$tag FAIL: phase2 free+used+invalid $ph2_sum != total units $tot_units"
        fail=$((fail+1)); failures+=("$fs/report/ph2sum")
    elif [ "$rep_sum" != "$tot_bytes" ]; then
        echo "$tag FAIL: files+dirs+bad+avail $rep_sum != total bytes $tot_bytes"
        fail=$((fail+1)); failures+=("$fs/report/sum")
    else
        echo "$tag ok (units=$tot_units free=$avail_units, files+dirs bytes reported)"
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
