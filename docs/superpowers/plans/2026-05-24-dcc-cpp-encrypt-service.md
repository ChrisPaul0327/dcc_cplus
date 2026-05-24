# DCC C++ Encrypt Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a baseline-compatible C++ service for the DCC08 high-concurrency dynamic masking benchmark.

**Architecture:** Keep the HTTP and CSV protocols identical to the Java baseline while replacing the internal path with column-oriented storage, precomputed mask values, scalar SM4 CBC encryption, tile rendering, ordered file writes, and delayed callback after close/rename.

**Tech Stack:** C++17, POSIX sockets, POSIX file I/O, Makefile, no external runtime dependencies.

---

### Task 1: Core Crypto And Data Tests

**Files:**
- Create: `tests/test_core.cpp`
- Create: `Makefile`

- [x] Write tests for the SM4 official block vector, baseline mask behavior, request JSON field ordering, and rendered CSV row ordering.
- [x] Run `make test` and confirm it fails before implementation because the core modules are missing.

### Task 2: Core Modules

**Files:**
- Create: `src/sm4.h`
- Create: `src/sm4.cpp`
- Create: `src/mask.h`
- Create: `src/mask.cpp`
- Create: `src/request.h`
- Create: `src/request.cpp`
- Create: `src/data_store.h`
- Create: `src/data_store.cpp`

- [x] Implement SM4 key schedule and block encryption.
- [x] Implement SM4 CBC with PKCS#7 padding and uppercase hex output.
- [x] Implement baseline-equivalent mask handling for ASCII and UTF-8 Chinese text.
- [x] Implement minimal JSON request parsing for the competition payload.
- [x] Implement CSV load, column storage, mask precompute, field resolution, and rendered row append.
- [x] Run `make test` and confirm all core tests pass.

### Task 3: Service Runtime

**Files:**
- Create: `src/job_scheduler.h`
- Create: `src/job_scheduler.cpp`
- Create: `src/http_server.h`
- Create: `src/http_server.cpp`
- Create: `src/main.cpp`

- [x] Implement runtime configuration from environment variables.
- [x] Implement lazy CSV load and asynchronous warmup after successful `/health`.
- [x] Implement queued `/encrypt` jobs, tile rendering with compute threads, temp-file write, close, atomic rename, and callback.
- [x] Implement minimal HTTP server for `/health` and `/encrypt`.
- [x] Build `dcc_encrypt`.

### Task 4: Verification And Packaging

**Files:**
- Create: `README.md`
- Create: `command.txt`

- [x] Run `make clean && make dcc_encrypt test`.
- [x] Run local `/health`.
- [x] Run local `/encrypt` with callback disabled.
- [x] Confirm output row count is 300000 and field order matches the request.
- [x] Document build, runtime variables, and submit command template.
