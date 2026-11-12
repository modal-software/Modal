#!/bin/sh
# bench.sh (v2 tool): the numbers of docs/p.md, measured the same way every time. `make bench`.
#
# For each program: checking (warm, in one process), the emit phase, compiling to an executable,
# running it, `--run`, the sizes and memory `--stats` reports, and the compiler's peak RSS and
# page faults. Times are the best of N runs (bestof.c), in ms. The output is a markdown table,
# to paste into docs/p.md and docs/context.md before and after a step.
#
#   sh src/v2/tools/bench.sh [N]      N runs per time (default 30); BIN=./modal by default
#
# `big` is generated here (deterministic): 54,000 small functions in chains of 100, about 3 MB,
# for the compiler's speed at scale.
BIN=${BIN:-./modal}
N=${1:-30}
W=${TMPDIR:-/tmp}/modal-bench.$$
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
BESTOF="$W/bestof"
${CC:-cc} -O2 -std=c17 ./src/v2/tools/bestof.c -o "$BESTOF" || exit 1

# big.modal: f<i> calls f<i-1> inside each chain of 100; main calls the end of a few chains.
awk 'BEGIN {
    print "print := import(\"std\").io.print"
    for (i = 0; i < 54000; i++) {
        printf "f%d(a: u64, b: u64): u64 {\n", i
        if (i % 100 == 0) printf "    return (a + b) * 3 - a\n"
        else printf "    return f%d(b, a + %d) + 1\n", i - 1, i % 7
        print "}"
    }
    print "main(): u8 {"
    print "    print(\"big\", f99(1, 2), f26999(3, 4), f53999(5, 6))"
    print "    return 0"
    print "}"
}' > "$W/big.modal"

# best of $N of the `--time` line for a phase, of a full compile to an executable
phase_best() { # phase file
    i=0
    while [ $i -lt "$N" ]; do
        i=$((i + 1))
        $BIN --backend native -o "$W/p$i" "$2" --time 2>/dev/null | awk -v p="$1" '$1 == p { print $2 }'
        rm -f "$W/p$i"
    done | sort -n | head -1
}

# the compiler's peak RSS (MB) and page faults, for one compile to an executable
rss_faults() { # file
    /usr/bin/time -l $BIN --backend native -o "$W/rss" "$1" 2>&1 >/dev/null |
        awk '/maximum resident set size/ { rss = $1 / 1048576 }
             /page reclaims/ { f += $1 } /page faults/ { f += $1 }
             END { if (rss) printf "%.1f %d", rss, f; else printf "- -" }'
}

for i in 1 2 3; do # warm-up: the first program measured would pay for the cold machine
    $BIN --backend native -o "$W/warm" examples/secret.modal >/dev/null 2>&1
    rm -f "$W/warm"
done

echo "| program | check warm | emit | -o | exec | --run | code B | file B | RAM B | memory used / taken KB | RSS MB | faults |"
echo "|---|---|---|---|---|---|---|---|---|---|---|---|"
for f in examples/secret.modal examples/run/hello.modal examples/run/output.modal \
    examples/run/structs.modal examples/run/patterns.modal "$W/big.modal"; do
    name=$(basename "$f" .modal)
    reps=200
    runs=$N
    [ "$name" = big ] && reps=20 && runs=$(( N < 10 ? N : 10 ))
    check=$($BIN --time --repeat $reps "$f" 2>/dev/null | awk '$1 == "total" { print $2 }')
    emit=$(phase_best emit "$f")
    compile=$("$BESTOF" "$runs" $BIN --backend native -o "$W/exe-$name" "$f")
    # a fresh path, so its first-launch validation (docs/p.md §2.2) is not in the time
    $BIN --backend native -o "$W/run-$name" "$f" >/dev/null 2>&1
    "$W/run-$name" >/dev/null 2>&1
    exec=$("$BESTOF" "$runs" "$W/run-$name")
    run=$("$BESTOF" "$runs" $BIN --backend native --run "$f")
    stats=$($BIN --backend native -o "$W/st-$name" "$f" --stats 2>/dev/null)
    code=$(echo "$stats" | awk '$1 == "memory" { print $3 }')
    ram=$(echo "$stats" | awk '$1 == "memory" { print $6 }')
    used=$(echo "$stats" | awk '$1 == "memory:" { print $2 }')
    taken=$(echo "$stats" | awk '$1 == "memory:" { print $6 }')
    bytes=$(wc -c < "$W/st-$name" | tr -d ' ')
    set -- $(rss_faults "$f")
    echo "| $name | $check | $emit | $compile | $exec | $run | $code | $bytes | $ram | $used / $taken | $1 | $2 |"
done
echo
echo "process start (/usr/bin/true): $("$BESTOF" "$N" /usr/bin/true) ms; $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)"
