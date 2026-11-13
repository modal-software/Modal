CC = gcc
# CFLAGS = -Wall -O3 -Wextra -g -std=c17 -MMD -MP -I ./ -I src/v1/compiler -I src
TARGET = jma
LIBS = -lm -lpthread -ldl

SRCS = ./src/v2/main.c ./src/v2/compiler/cx.c ./src/v2/compiler/syntax/scanner.c ./src/v2/compiler/syntax/parser/parser.c ./src/v2/compiler/syntax/parser/parse_decl.c ./src/v2/compiler/syntax/parser/parse_expr.c ./src/v2/compiler/syntax/parser/parse_stmt.c ./src/v2/compiler/ast/ast.c ./src/v2/compiler/ast/symbol.c ./src/v2/compiler/sema/resolve.c ./src/v2/compiler/sema/module.c ./src/v2/compiler/sema/check.c ./src/v2/compiler/sema/types.c ./src/v2/compiler/sema/layout.c ./src/v2/compiler/sema/cache.c ./src/v2/compiler/sema/comptime.c ./src/v2/compiler/backend/registry.c ./src/v2/compiler/backend/c/emit.c ./src/v2/compiler/backend/native/emit.c ./src/v2/compiler/backend/native/macho.c ./src/v2/compiler/backend/native/elf.c ./src/v2/compiler/backend/native/run.c ./src/v2/compiler/backend/native/regalloc.c ./src/v2/compiler/backend/native/arch_arm64.c ./src/v2/compiler/backend/native/arch_x86_64.c ./src/v2/compiler/mir/lower.c ./src/v2/compiler/mir/opt.c
NATIVE = ./src/v2/compiler/backend/native
HDRS = $(wildcard ./src/v2/compiler/*.h ./src/v2/compiler/*/*.h ./src/v2/compiler/*/*/*.h)
# -flto: the sources are optimized as one program (about 7% faster: hot helpers inline
# across files, as in TCC's one-source build).
CFLAGS = -Wall -Wextra -O2 -flto -g -std=c17 -I src
OBJS = $(SRCS:.c=.o)

OUT_DIR := ./out
	
# Paths
PREFIX ?= /usr/local
BINDIR = ${PREFIX}/bin

# %.o: %.c
# 	$(CC) $(CFLAGS) -c $< -o $@

modal: $(SRCS) $(HDRS) $(NATIVE)/stencils_arm64.h $(NATIVE)/stencils_x86_64.h
	$(CC) $(CFLAGS) $(SRCS) -o ./modal

# Native backend: the stencils (machine-code templates) are compiled by clang for the target CPU
# and turned into a header by stencilgen. The header is kept in the tree, so building Modal
# needs clang only after stencils.c changes. ELF objects are used on every host: their
# relocations are simple to read, and the machine code is the same.
STENCIL_CC = clang
STENCIL_FLAGS = -ffreestanding -fno-pic -O2 -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -ffunction-sections -fno-jump-tables -Wall -Wextra
# arm64: holes are movz/movk immediates (large code model); x18 is the platform register
# on Apple and is never touched. x86-64: holes are 32-bit immediates (small code model).
STENCIL_ARM64 = --target=aarch64-unknown-linux-gnu -mcmodel=large -ffixed-x18
STENCIL_X86_64 = --target=x86_64-unknown-linux-gnu

stencils: $(NATIVE)/stencils_arm64.h $(NATIVE)/stencils_x86_64.h

./src/v2/tools/stencilgen: ./src/v2/tools/stencilgen.c
	$(CC) -O2 -Wall -Wextra -std=c17 $< -o $@

$(NATIVE)/stencils_%.h: $(NATIVE)/stencils.c ./src/v2/tools/stencilgen
	$(STENCIL_CC) $(STENCIL_$(shell echo $* | tr a-z A-Z)) $(STENCIL_FLAGS) -c $< -o $(NATIVE)/stencils_$*.o
	./src/v2/tools/stencilgen $(NATIVE)/stencils_$*.o $* > $@.tmp && mv $@.tmp $@
	rm -f $(NATIVE)/stencils_$*.o

run: modal
	clear; ./modal --run ./examples/secret.modal

run_test: modal
	clear; ./modal ./examples/secret.modal

test: modal
	sh ./src/v2/tests/run.sh

# The numbers of docs/p.md (times best of N, sizes, memory, RSS): a markdown table.
bench: modal
	sh ./src/v2/tools/bench.sh $(N)

# Random programs, run by every backend: output and status must agree (src/v2/tools/fuzz.py).
fuzz: modal
	python3 -I ./src/v2/tools/fuzz.py $(or $(N),100) $(or $(SEED),1)

# One program on every backend: each line must show the same status and output hash.
backends: modal
	sh ./src/v2/tools/backends.sh $(F)

# The compiler with AddressSanitizer and UndefinedBehaviorSanitizer, as ./modal-asan. Run the
# suite with it: BIN=./modal-asan make test (and the fuzzer the same way).
asan: $(SRCS) $(HDRS) $(NATIVE)/stencils_arm64.h $(NATIVE)/stencils_x86_64.h
	clang -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=c17 -I src $(SRCS) -o ./modal-asan

test_parser3: src/v2/tests/ll1_parsing_3.c
	$(CC) -Wall -Wextra -O2 -std=c17 -pthread src/v2/tests/ll1_parsing_3.c -o src/v2/tests/test_parser3

bench3: test_parser3
	@echo "== static baseline =="
	./src/v2/tests/test_parser3 static
	@echo "== dynamic queue =="
	./src/v2/tests/test_parser3 dyn
	@echo "== dynamic oversubscribed =="
	./src/v2/tests/test_parser3 dyn 32

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) modal modal-v2 modal-asan

-include $(OBJS:.o=.d)
