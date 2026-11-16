# Project Modal: General Ecosystem Platform (GEP)

This repository implements Modal, a language runtime focusing on performance and low-level control. The core design follows an "Explicit" philosophy, providing low-level language features (like manual memory control) while offering abstraction layers. The goal is a General Ecosystem Platform (GEP).

## 🛠️ Dev Environment & Setup

The project is primarily developed in C, targeting x86-64 architectures. It uses a multi-version source structure (`src/v1/` and `src/v2/`) that must be managed by the build system.

**Build Command:**
\`make\` (This compiles all sources into the \`./modal\` executable).

**Testing/Benchmarking:**
Tests are highly version-specific (e.g., \`make test_parser3\`). Use \`make benchN\` targets for phased benchmarks, which execute specific compiler tests in sequence.

## 📚 Code Conventions

*   **Versioning:** Source code is partitioned by version in the directory structure (\`src/v1/\`, \`src/v2/\`).
*   **Design Pattern:** The design is explicitly focused on control, featuring switchable memory management models (manual, GC, borrow-checker concepts).
*   **Naming:** File and target naming is granular, reflecting specific parser components (e.g., \`syntax/parser/parse_stmt.c\`).

## ⚠️ Known Pitfalls & Constraints

*   **AVX2 Compatibility:** When compiling, be aware of the comment in the Makefile regarding AVX2. The code must utilize runtime dispatch for target \`"avx2"\` sections to maintain safety across all x86-64 processors. Do not assume native AVX2 support without checking the tagged section.
*   **Source Structure:** The Makefile relies on a very specific, hand-managed set of source files. Do not attempt large-scale automated file changes without understanding the `v1` vs `v2` partitioning.
*   **Tooling:** The project does not use a package manager (like Cargo or npm). Dependencies are linked via system libraries and explicit CFLAGS/LIBS.
