# High-Performance Limit Order Book (C++20)

A low-latency Limit Order Book (LOB) matching engine built in modern C++20. 

This project is currently in **Phase 3** of development, establishing the baseline performance using standard STL containers (`std::map`, `std::list`). Subsequent phases will replace standard containers with a custom intrusive memory pool and cache-aligned contiguous data structures to eliminate heap allocations on the critical path.

## Architecture (Baseline)
* **Matching Engine:** Price-time priority matching.
* **Containers:** `std::map` for price levels, `std::list` for order queues.
* **Order Index:** `std::unordered_map` for O(1) cancel/reduce lookups.



### Benchmark Environment
* **OS:** Windows 11 (x86_64)
* **Compiler:** GCC 15.2.0 (MinGW)
* **Optimization:** `-O3 -fno-omit-frame-pointer`
* **Thread Affinity:** Unpinned (OS scheduled)