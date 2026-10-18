# C++20 Limit Order Book & Matching Engine

[![CI](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/HardikGorani-dev/Limit_Order_Book_cpp/actions/workflows/ci.yml)

A price-time priority (FIFO) financial limit order book and matching engine written in standard C++20.

**Current Status:** Core STL baseline engine, 24-case typed specification suite, 10M-operation differential fuzzer against a flat-vector reference model, and per-operation latency/allocation benchmark harness are complete. Intrusive slab memory pool integration is in progress.

---

## Build, Test, and Run

### 1. Build (Release Default)
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build