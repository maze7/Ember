# Ember

Ember is a modular C++20 3D game engine designed for high-performance rendering and simulation, with explicit resource ownership and predictable memory use.

## Design

- Explicit ownership, resource lifetimes, and synchronization.
- Memory budgets defined at boot, with allocations grouped by lifetime.
- Fiber-based job scheduling, with blocking IO kept off worker threads.
- Data-oriented storae and batch processing on hot paths.
- Small modules with clear dependencies and explicit lifecycles.
- Cooked runtime data and hot reload for fast iteration.
- Profiling, validation, and diagnostics built into the foundation.

# Status

Ember is under active development. The current foundation includes memory, jobs, filesystem, and SDL3 platform and input support. Rendering and other subsystems are being rebuilt incrementally.

## Build

Requires CMake 4.3+, Ninja, a C++20 toolchain, and SDL3's platform dependencies.

From the Ember directory:

```sh
git submodule update --init --recursive
cmake --preset debug
cmake --build --preset debug
```

Use `release` instead of `debug` for a release build.
