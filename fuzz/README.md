# Fuzzing

libFuzzer-based targets for primechain's binary decoders. Off by default
(`PRIMECHAIN_BUILD_FUZZERS`) since `-fsanitize=fuzzer` is clang-only and the
project's default/tested build commonly uses gcc -- this needs its own
build directory, not a flag on the normal one.

## What's covered so far

Three targets, one per canonical decode function:

- `fuzz-transaction-decode` -- `primechain::protocol::deserializeTransaction`
- `fuzz-composite-record-decode` -- `deserializeCompositeRecord`
- `fuzz-prime-record-decode` -- `deserializePrimeRecord`

All three are pure bytes-in/struct-out functions: no sockets, no
filesystem, no global state. That's deliberate -- it's real attack surface
(a transaction is embedded inside a submitted record; a peer can send
either directly), and it means these can run at full in-process libFuzzer
speed with zero server/workdir scaffolding, and any crash they find is
directly reproducible from the saved input file alone.

This intentionally does **not** yet cover the live network/wire-command
parser (`sync_server.cpp`'s line-based `GET_*`/`SUBMIT_*` dispatch) --
that's real, higher-priority attack surface too (arguably higher, since
it's what an unauthenticated remote peer can reach first), but fuzzing it
cleanly needs either refactoring the line-parsing logic out of the
network I/O loop into something callable in-process, or a
network-facing/AFL-persistent-mode harness -- more setup than these three,
and deliberately left for a follow-up rather than attempted alongside this
first batch (see the engineering roadmap: "do not fuzz everything at
once").

## Building and running

```sh
cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DPRIMECHAIN_BUILD_FUZZERS=ON
cmake --build build-fuzz --target fuzz-transaction-decode fuzz-composite-record-decode fuzz-prime-record-decode

mkdir -p fuzz/corpus/transaction fuzz/corpus/composite-record fuzz/corpus/prime-record
./build-fuzz/fuzz-transaction-decode fuzz/corpus/transaction
./build-fuzz/fuzz-composite-record-decode fuzz/corpus/composite-record
./build-fuzz/fuzz-prime-record-decode fuzz/corpus/prime-record
```

Each binary is a standard libFuzzer executable -- `-help=1` for the usual
flags (`-max_total_time=N`, `-jobs=N`, `-max_len=N`, etc). A crash is saved
as `crash-<hash>` in the working directory; rerun the binary with that
single file as the argument to reproduce it directly, no server needed.

## Expected properties (per the roadmap)

- no crash, no sanitizer-flagged undefined behavior, no unbounded
  allocation from a small input,
- a successfully decoded value can always be re-encoded (and hashed)
  without crashing, whether or not it round-trips byte-for-byte,
- rejection of malformed input must be quiet (return `std::nullopt` +
  an error string), never a crash.

Every reproducible finding should become a regression test (a fixed input
added to one of the corpus directories above, or a dedicated CMake test
case), per the roadmap's bug-finding discipline -- reproduce, minimize,
classify, only then fix.
