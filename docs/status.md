# Tachyon status

Tachyon is a portfolio/learning implementation of a single-symbol matching
engine plus a local dashboard demo. It is useful for studying price-time
priority, matching semantics, and hot-path allocation discipline.

It is not a production exchange service:

- no persistence, audit log, recovery, or replay;
- no network order entry protocol or authentication;
- no risk checks, market data distribution guarantees, or operational runbooks;
- dashboard traffic is a local demo stream, not a live market feed.

The recommended confidence checks before sharing changes are:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
bash scripts/smoke_demo.sh
```

If CMake is not available locally, `bash scripts/smoke_demo.sh` still performs a
bounded compiler/demo check with the system C++17 compiler.
