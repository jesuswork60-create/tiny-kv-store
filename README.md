# tiny-kv-store

An embedded key-value store for C++17, built around an append-only log in the style of Bitcask. Writes go to the end of a segment file, an in-memory sorted map says where each key's latest value lives, and a compaction step rewrites the live data to reclaim the space taken by overwrites and deletes. It comes as a static library plus a small command-line tool.

It has no dependencies beyond the C++ standard library and POSIX. This is a small project and has not been used for anything serious, so treat it accordingly.

What it does:

- `put`, `get`, `del`, and atomic batches of puts and deletes.
- Ordered scans by prefix or key range, through iterators that keep seeing the data as it was when they were created, even across compaction.
- Crash recovery: a torn or corrupt tail on the newest segment is cut off at open, and a batch is either all there or all gone.
- A CRC32 on every record, checked on every read.
- Hint files, so that opening a store doesn't have to read all the values.
- `verify` and `dump` tools for looking at a store's files.

## Install

You need a C++17 compiler and CMake 3.20 or newer.

```sh
cmake -S . -B build
cmake --build build -j1
```

This produces `build/libtinykv.a` and the tool `build/tinykv`. To use the library from another CMake project, add this folder with `add_subdirectory` and link the `tinykv` target.

## Usage

From the command line:

```console
$ alias tinykv=build/tinykv
$ tinykv --dir demo-data put city Lisbon
$ tinykv --dir demo-data put user:1 Ada
$ tinykv --dir demo-data put user:2 Grace
$ tinykv --dir demo-data get city
Lisbon
$ tinykv --dir demo-data scan user:
user:1	Ada
user:2	Grace
$ printf 'put user:3 Joan\ndel city\n' | tinykv --dir demo-data batch
applied 2 operations
$ tinykv --dir demo-data keys
user:1
user:2
user:3
$ tinykv --dir demo-data compact
kept 3 keys, 206 -> 125 bytes, 1 old segment(s) removed
$ tinykv --dir demo-data verify
2 segment(s), 3 records, 3 live keys, 1 hints checked: ok
```

Run `tinykv` with no arguments for the full list of commands and options. It exits with 0 on success, 1 when a key is absent, 2 for a usage error, and 3 for a store error or a failed `verify`.

From C++:

```cpp
#include <iostream>

#include "tinykv/store.h"

int main() {
  auto store = tinykv::Store::open("demo-lib");

  store->put("user:1", "Ada");
  store->put("user:2", "Grace");

  tinykv::WriteBatch batch;
  batch.put("user:3", "Joan");
  batch.del("user:1");
  store->write(batch);

  if (auto value = store->get("user:2")) std::cout << *value << "\n";

  tinykv::ScanRange range;
  range.prefix = "user:";
  for (auto it = store->scan(range); it.valid(); it.next()) {
    std::cout << it.key() << " = " << it.value() << "\n";
  }

  store->compact();
}
```

Errors are thrown as `tinykv::Error`, which carries an `ErrorCode` (`Io`, `Corruption`, `Locked`, `InvalidArgument` or `Closed`). `Options` in `include/tinykv/options.h` sets the segment size, the value size limit and how often to fsync. By default it fsyncs on every write, which is safe and slow.

[docs/format.md](docs/format.md) describes the files on disk and what happens in a crash.

## Running the tests

```sh
cmake -S . -B build && cmake --build build -j1 && ctest --test-dir build --output-on-failure
```

## Known limitations

- Every key is held in memory in a `std::map`, with its location, so the whole key set has to fit in RAM.
- Compaction is manual, always covers all the sealed segments, and blocks readers and writers until it finishes. If it meets a corrupt live record it stops without removing anything, so a store with one bad value can't be compacted until that key is deleted or overwritten.
- Creating an iterator copies the keys in its range, so a scan of the whole store costs memory proportional to the store.
- Values are read in full. There is no streaming, and each `get` reads the whole record to check its CRC.
- In the newest segment, a flipped byte in the middle of the file can't be told apart from a torn write, so recovery cuts off everything after it.
- A valid hint file means the segment behind it isn't read at open, so damage in an older segment may only show up on read or in `verify`.
- Salvage stops at the first damage in a segment and loses the rest of that file, including any deletes, so a deleted key can come back.
- Only one process can have a store open at a time, and there is no read-only open mode. `verify` needs the lock, so it refuses a store that's in use. `dump` doesn't take the lock, so on a live store it can show a half-written last record. The lock is a `flock`, which is advisory and may not work on network filesystems.
- POSIX only. It has only been built and run on Linux.
- No compression, no expiry and no reverse iteration.
