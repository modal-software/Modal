#!/bin/sh
# backends.sh (v2 tool): one program on every backend, side by side. `make backends F=file.modal`.
#
# Prints, per backend, the exit status and a hash of everything the program wrote (output and
# error output). Every line must show the same status and hash: C is the reference. The outputs
# stay in $W (printed at the end) to diff when they do not agree.
#   C (cc)   arm64 (native, in memory)   x86-64 (Rosetta on Apple Silicon)   Linux arm64 / x86-64 ELF
BIN=${BIN:-./modal}
F=$1
[ -n "$F" ] || { echo "usage: sh src/v2/tools/backends.sh file.modal" >&2; exit 2; }
W=${TMPDIR:-/tmp}/modal-backends
mkdir -p "$W"
hash() { cksum < "$1" | cut -d' ' -f1; }
line() { printf '%-8s status %-3s output %s\n' "$1" "$2" "$(hash "$3")"; }

$BIN --run "$F" > "$W/c.out" 2>&1; line c $? "$W/c.out"
if [ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ]; then
    $BIN --backend native --run "$F" > "$W/arm64.out" 2>&1; line arm64 $? "$W/arm64.out"
fi
if [ "$(uname -m)" = x86_64 ] || arch -x86_64 /usr/bin/true 2>/dev/null; then
    $BIN --backend x86_64 --run "$F" > "$W/x86_64.out" 2>&1; line x86_64 $? "$W/x86_64.out"
fi
for cpu in arm64 x86_64; do
    runner=""
    if [ "$(uname -s)" = Darwin ]; then
        [ $cpu = x86_64 ] && ! arch -x86_64 /usr/bin/true 2>/dev/null && continue
        ${CC:-cc} -arch $cpu -O2 -std=c17 ./src/v2/tools/elfrun.c -o "$W/elfrun-$cpu" 2>/dev/null || continue
        runner="$W/elfrun-$cpu"
        [ $cpu = x86_64 ] && runner="arch -x86_64 $W/elfrun-$cpu"
    elif [ "$(uname -m)" != "$cpu" ] && [ "$(uname -m)-$cpu" != aarch64-arm64 ]; then
        continue
    fi
    if $BIN --target $cpu-linux -o "$W/elf-$cpu" "$F" > "$W/elf-$cpu.out" 2>&1; then
        $runner "$W/elf-$cpu" > "$W/elf-$cpu.out" 2>&1
    fi
    line elf-$cpu $? "$W/elf-$cpu.out"
done
echo "(outputs in $W)"
