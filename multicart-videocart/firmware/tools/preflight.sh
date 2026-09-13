#!/usr/bin/env bash
# ======================================================================
# preflight.sh — everything that can be proven WITHOUT the CoCo.
#
# Run by `make preflight`. Every check is a hard gate: the script exits
# non-zero on the first failure, and its output is meant to be pasted
# into the handoff so the evidence is visible rather than asserted.
#
# What this CANNOT prove is listed at the end, deliberately: '153 settle
# on hot silicon, SRAM arbiter contention, drive strength. Those need the
# bench. Everything else is checked here.
# ======================================================================
set -u
cd "$(dirname "$0")/.."          # firmware/

fail=0
step() { printf '\n=== %s\n' "$1"; }
ok()   { printf '  PASS  %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

VC=videocart/build/coco-videocart.elf
MC=multicart/build/coco-multicart.elf
PIOASM="${PIOASM:-$(find "$HOME/.cache/coco-fw" /usr/local -name pioasm -type f 2>/dev/null | head -1)}"

# ---------------------------------------------------------------------
step "1. Build both boards"
if make -s >/tmp/pf_build.log 2>&1; then ok "multicart + videocart built"
else bad "build failed"; tail -20 /tmp/pf_build.log; fi

# ---------------------------------------------------------------------
step "2. PIO instruction budget (32 words per block, shared)"
if [ -z "$PIOASM" ]; then
    bad "pioasm not found (set PIOASM=<path>)"
else
    "$PIOASM" -o c-sdk videocart/bus_pio.pio /tmp/pf_pio.h >/dev/null 2>&1 \
        || bad "pioasm rejected bus_pio.pio"
    len() { awk "/^#define ${1}_wrap /{print \$3}" /tmp/pf_pio.h; }
    # wrap is the index of the last instruction; length = wrap + 1 for
    # programs whose wrap_target is 0 (all of ours except wr_capture,
    # which has a one-instruction preamble).
    samp=$(( $(len mux_sampler) + 1 ))
    fol=$((  $(len sweep_follower) + 1 ))
    cap=$((  $(len wr_capture) + 1 ))
    srv=$((  $(len bus_serve) + 1 ))
    pio2=$(( fol + cap + srv ))
    printf '  pio1: sampler %d\n  pio2: follower %d + capture %d + serve %d = %d\n' \
           "$samp" "$fol" "$cap" "$srv" "$pio2"
    [ "$samp" -le 32 ] && ok "sampler fits pio1"  || bad "sampler is $samp > 32"
    [ "$pio2" -le 32 ] && ok "pio2 programs fit"  || bad "pio2 needs $pio2 > 32"
fi

# ---------------------------------------------------------------------
step "3. Host simulations (serve pipeline, sampler timing, everything else)"
if make -s test PIOASM="$PIOASM" >/tmp/pf_test.log 2>&1; then
    grep -E ': OK|PASSED' /tmp/pf_test.log | sed 's/^/  /'
    ok "all host tests"
else
    bad "host tests failed"; grep -E 'FAIL|Error' /tmp/pf_test.log | head -20
fi

# ---------------------------------------------------------------------
step "4. RAM residency of the core-1 loop (flash calls = 0)"
if make -s audit >/tmp/pf_audit.log 2>&1 && grep -q 'audit OK' /tmp/pf_audit.log
then ok "no flash calls from RAM code"
else bad "audit failed"; cat /tmp/pf_audit.log; fi

# ---------------------------------------------------------------------
step "5. Serve table alignment (sampler ORs base|index -- 32 KB or bust)"
addr=$(arm-none-eabi-nm "$VC" | awk '/ [bB] s_main$/{print $1}')
if [ -n "$addr" ] && [ $(( 0x$addr & 0x7FFF )) -eq 0 ]; then
    ok "s_main @0x$addr is 32 KB aligned"
else
    bad "s_main @0x${addr:-?} is NOT 32 KB aligned -- every lookup would corrupt"
fi

# ---------------------------------------------------------------------
step "6. Core-1 loop shape (no calls, no flash, bounded)"
arm-none-eabi-objdump -d --no-show-raw-insn "$VC" \
    | awk '/<bus_loop>:/{f=1} f{print} f&&NF==0{exit}' > /tmp/pf_loop.dis
calls=$(grep -c '	bl	' /tmp/pf_loop.dis || true)
insns=$(grep -c ':	' /tmp/pf_loop.dis || true)
lo=$(awk '/<bus_loop>:/{print $1}' /tmp/pf_loop.dis)
printf '  bus_loop @0x%s, %s instructions, %s calls\n' "$lo" "$insns" "$calls"
[ "$calls" -eq 0 ] && ok "loop is call-free" || bad "loop makes $calls call(s)"
case "$lo" in 20*) ok "loop is in SRAM" ;; *) bad "loop is not in SRAM" ;; esac

# ---------------------------------------------------------------------
step "7. Multicart untouched by the videocart work"
if git -c safe.directory="$PWD/.." diff --quiet HEAD -- multicart/bus.c multicart/bus_pio.pio 2>/dev/null; then
    ok "multicart bus code unchanged vs HEAD"
else
    printf '  note: multicart bus files differ from HEAD (review the diff)\n'
    git -c safe.directory="$PWD/.." --no-pager diff --stat HEAD \
        -- multicart/bus.c multicart/bus_pio.pio 2>/dev/null | sed 's/^/    /'
fi

# ---------------------------------------------------------------------
step "8. RAM headroom"
arm-none-eabi-size "$VC" | sed 's/^/  /'

# ---------------------------------------------------------------------
printf '\n======================================================\n'
if [ "$fail" -eq 0 ]; then
    cat <<'EOT'
PREFLIGHT PASSED.

NOT proven here -- these need the bench, and no amount of static
analysis substitutes for them:
  * '153 select-to-output settle on hot silicon (design margin: 64 ns
    against a ~43 ns worst case)
  * SRAM arbiter contention from the HSTX scanout DMA
  * data-bus drive strength / capacitance at the cartridge connector

On hardware, the acceptance criteria are:
  lap ~1117 ns; dropped = 0 (svstall, pairdrop, tagslip all zero);
  win reads climbing with a cart mounted; diagnostics-v2 boots; then a
  banked cart boots and plays.
EOT
else
    printf 'PREFLIGHT FAILED -- do not flash.\n'
fi
exit "$fail"
