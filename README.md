# fastconstmap

[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![Test](https://github.com/lemire/fastconstmap/actions/workflows/test.yml/badge.svg)](https://github.com/lemire/fastconstmap/actions/workflows/test.yml)

Fast, immutable, compact map from strings to 64-bit integers — for Python.

`fastconstmap` is a C implementation of the [binary fuse filter][bff]
construction (a static perfect-hash-like structure), exposed to Python.
Given a `dict[str, int]` at build time, you get back a lookup object that:

- uses **~9 bytes per key** (or ~18 with missing-key detection),
- answers a lookup in **one xxhash call plus three array reads**,
- is **immutable** and **serializable** to bytes / a file,
- exposes both a single-key API (`m[key]`) and a **batched** API
  (`m.get_many([...])`, `m.get_many_into([...], buf)`) that amortises Python-C
  call overhead and overlaps the memory accesses of a whole block of keys.

This package is a C port of the Go library
[`github.com/lemire/constmap`][constmap]. It vendors
[xxHash](https://github.com/Cyan4973/xxHash) (BSD-2) for string hashing.

## Installation

```
pip install fastconstmap
```

To build from source you need a C compiler. There are no Python runtime
dependencies.

## Usage

```python
from fastconstmap import ConstMap, VerifiedConstMap

d = {"apple": 100, "banana": 200, "cherry": 300}

# Variant 1: minimal memory, no missing-key detection.
m = ConstMap(d)
m["apple"]                  # -> 100
m.get_many(["banana", "cherry"])  # -> [200, 300]
m["grape"]                  # undefined value!  use VerifiedConstMap if you care.

# Variant 2: dict-like, detects keys not in the original mapping.
vm = VerifiedConstMap(d)
vm["apple"]                 # -> 100
vm.get("grape")             # -> None
vm.get("grape", -1)         # -> -1
"grape" in vm               # -> False
vm["grape"]                 # raises KeyError
vm.get_many(["banana", "grape"], default=-1)  # -> [200, -1]

# Batches can also be written straight into a buffer you own — no list of
# Python ints is built.
import array
out = array.array("Q", [0]) * 3
m.get_many_into(["apple", "banana", "cherry"], out)   # -> 3; out == [100, 200, 300]

# Either kind can be saved and loaded.
m.save("mymap.cmap")
m2 = ConstMap.load("mymap.cmap")

# ... or as raw bytes.
blob = m.to_bytes()
m3 = ConstMap.from_bytes(blob)
```

### Choosing between `ConstMap` and `VerifiedConstMap`

| | `ConstMap` | `VerifiedConstMap` |
|--|--|--|
| Bytes per key | ~9 | ~18 |
| Lookup of present key | value | value |
| Lookup of missing key | **undefined garbage** | `KeyError` / `default` / `None` |
| Best if | you always look up known keys | you need dict-like semantics |

The false-positive rate of `VerifiedConstMap` (a missing key wrongly
reported as present) is roughly 2⁻⁶⁴, which is negligible in practice.

## Batched lookups

`get_many` takes an iterable of keys and returns a list of values, in the same
order:

```python
cm.get_many(["apple", "banana", "cherry"])          # -> [100, 200, 300]
vm.get_many(["banana", "grape"], default=-1)        # -> [200, -1]
```

`get_many_into` is the same lookup writing 64-bit words into a buffer you own,
so a repeated batch allocates nothing at all — no list, and no Python `int` per
key. The buffer can be an `array("Q")`, a numpy `uint64` array, a
`SharedMemory` block, or any other writable buffer of at least `8 * len(keys)`
bytes. It returns the number of values written:

```python
import array
out = array.array("Q", [0]) * 4096
for batch in batches:
    n = cm.get_many_into(batch, out)
    use(out[:n])
```

`VerifiedConstMap.get_many_into` writes `fastconstmap.NOT_FOUND` (`2**64 - 1`)
for a key that was not in the original mapping — a buffer of raw words has no
room for a Python default.

A batch is faster than a loop over single lookups because it hashes a block of
eight keys before gathering any values. That lets the three array reads of all
eight keys be in flight at once, instead of each key's loads waiting behind the
hashing of the key before it. A lookup is memory-latency bound as soon as the
map outgrows the last-level cache, which is where the gain comes from.

### Measured

At the C level, 2000-key batches against a 1,000,000-key map, ns/key, medians
of three runs (`benchmarks/bench_batch.c`). *Cold* rotates through 64 distinct
random batches so the cache lines the lookups touch are not already resident;
*hot* replays one batch, so the touched region stays cache-resident and hashing
dominates instead.

| | | loop over lookup | `lookup_many` | |
|---|---|---|---|---|
| **Apple M4 Max** | `ConstMap` cold | 5.5 | **3.8** | 30% |
| | `ConstMap` hot | 4.2 | **3.1** | 27% |
| | `VerifiedConstMap` cold | 9.6 | **7.8** | 19% |
| | `VerifiedConstMap` hot | 5.4 | **5.2** | 4% |
| **Xeon Gold 6548N** | `ConstMap` cold | 12.6 | **10.4** | 18% |
| | `ConstMap` hot | 6.3 | **5.0** | 21% |
| | `VerifiedConstMap` cold | 15.9 | 16.6 | −5% |
| | `VerifiedConstMap` hot | 7.9 | **6.7** | 15% |

The one case where batching does not pay is `VerifiedConstMap` in the cold
regime on the Xeon. That map probes two arrays per lookup, so a fresh batch is
bound by a memory latency the overlapping cannot hide. The Go original reports
the same result on the same processor.

Through the Python API, ns/key on the same batches — 0.8.0 is the previous
per-key loop, 0.9.0 the batched path:

| | | 0.8.0 | 0.9.0 | `get_many_into` |
|---|---|---|---|---|
| **Apple M4 Max** | `ConstMap` cold | 30.7 | **20.6** | **14.2** |
| | `ConstMap` hot | 16.4 | **14.1** | **6.1** |
| | `VerifiedConstMap` cold | 35.5 | 35.3 | **23.9** |
| **Xeon Gold 6548N** | `ConstMap` cold | 34.9 | **25.5** | **16.5** |
| | `ConstMap` hot | 18.7 | **17.2** | **8.8** |
| | `VerifiedConstMap` cold | 37.6 | **31.9** | **23.3** |

What is left in the `get_many` column is the cost of boxing each value in a
Python `int` and building the list, which is why `get_many_into` — which does
neither — is another 30-60% faster again.

To reproduce:

```
cc -O3 -std=c11 -Isrc benchmarks/bench_batch.c src/constmap.c -lm -o bench_batch
./bench_batch 1000000
```

## Keys and values

- **Keys** may be `str` or `bytes`. `str` is encoded as UTF-8 internally;
  lookups must use the same encoding to match.
- **Values** are 64-bit integers. We accept anything in
  `[-2**63, 2**64 - 1]`; negatives are stored via two's complement, so
  `m[k]` returns `2**64 - 1` for a value of `-1`. (To recover the signed
  reading, reinterpret bits yourself.)

Keys must be unique (Python dict semantics already guarantee this).
Construction raises `ValueError` in the extremely unlikely event of an
xxhash collision (~2⁻⁶⁴ per key pair).

## Sharing a map across processes (zero-copy)

A map's serialized form *is* its in-memory lookup array (plus a small
header). That means it can live in a
[`multiprocessing.shared_memory`](https://docs.python.org/3/library/multiprocessing.shared_memory.html)
block and be opened by any number of processes **without copying** — every
process reads the same physical pages.

Three methods make this work:

| Method | Purpose |
|--|--|
| `m.serialized_size()` | bytes needed to hold the serialized map |
| `m.write_into(buffer)` | serialize straight into a writable buffer (no intermediate `bytes`) |
| `ConstMap.from_buffer(buffer)` | open a **zero-copy** map that reads directly from `buffer` |

Producer — build once, publish into a **named** shared-memory block:

```python
from multiprocessing.shared_memory import SharedMemory
from fastconstmap import ConstMap

SHM_NAME = "fastconstmap_demo"

cm = ConstMap({f"key-{i}": i for i in range(1_000_000)})

shm = SharedMemory(create=True, size=cm.serialized_size(), name=SHM_NAME)
cm.write_into(shm.buf)
# keep `shm` alive (do not close/unlink) while consumers are running
```

Consumer — attach to the same name with no copy:

```python
from multiprocessing.shared_memory import SharedMemory
from fastconstmap import ConstMap

SHM_NAME = "fastconstmap_demo"

shm = SharedMemory(name=SHM_NAME)           # attach by name, no `create=`
cm = ConstMap.from_buffer(shm.buf)          # zero-copy: no per-process copy
cm["key-42"]                                # reads straight from shared memory
```

Choosing the name yourself (rather than letting `SharedMemory` generate
one) means consumers can hard-code it or read it from config — no need to
pass the auto-generated name around. Pick a unique name; creating a block
whose name already exists raises `FileExistsError`.

Notes and constraints:

- **`from_buffer` does not copy.** The returned map holds a reference to the
  buffer; the buffer (and, for shared memory, the `SharedMemory` object)
  must stay alive and **must not be closed or mutated** while the map is in
  use. Drop the map (`del cm`) before calling `shm.close()`.
- The map is **immutable** — the intended pattern is *write once in the
  producer, then only read in every process*. Concurrent readers need no
  locking.
- `from_buffer` verifies the magic bytes and the FNV-1a checksum, so a
  truncated or corrupt block raises `ValueError` rather than returning
  garbage.
- Requirements: a **little-endian** host (x86-64, ARM64, …) and an
  **8-byte-aligned** buffer. `SharedMemory.buf`, `bytes`, and `bytearray`
  all satisfy the alignment requirement; an offset slice of a buffer may
  not, in which case `from_buffer` raises `ValueError` — use
  `from_bytes()` (which copies) instead.
- `VerifiedConstMap` supports the same three methods.

`from_bytes()` remains available when you *want* an owned copy (or need to
load on a big-endian host): it copies the data and the resulting map owns
its memory independently of the source buffer.

## Benchmark

On an Apple M4 Max, with 1,000,000 string keys (`key-{i}-{hex}` shaped
strings). Lookups query every key exactly once, in random order, from a list
built in that order:

```
=== fastconstmap benchmark — n = 1,000,000 keys, python 3.14.5 ===

Construction:
  ConstMap.__init__                       0.145 s
  VerifiedConstMap.__init__               0.142 s
  dict(d)                                 0.004 s

Memory:
  ConstMap.nbytes                           9,043,968 bytes  (9.04 bytes/key)
  VerifiedConstMap.nbytes                  18,087,936 bytes  (18.09 bytes/key)
  dict (table+keys+values)                118,380,958 bytes  (118.38 bytes/key)
  ratio dict / ConstMap                  13.1x

Single lookup, every key once in random order (1,000,000 ops):
  dict[k]                                   336.1 ns/op  (0.336 s total)
  ConstMap[k]                                95.2 ns/op  (0.095 s total)
  VerifiedConstMap[k]                       112.3 ns/op  (0.112 s total)

Batched lookup, 2000 × 2000:
  dict comprehension (cold)                 148.7 ns/op  (0.30 ms/batch of 2000)
  ConstMap.get_many (cold)                   24.2 ns/op  (0.05 ms/batch of 2000)
  ConstMap.get_many (hot)                    15.6 ns/op  (0.03 ms/batch of 2000)
  ConstMap.get_many_into (cold)              14.2 ns/op  (0.03 ms/batch of 2000)
  ConstMap.get_many_into (hot)                6.1 ns/op  (0.01 ms/batch of 2000)
  VerifiedConstMap.get_many (cold)           33.7 ns/op  (0.07 ms/batch of 2000)
  VerifiedConstMap.get_many_into (cold)      23.9 ns/op  (0.05 ms/batch of 2000)

Serialization:
  ConstMap.to_bytes                       0.010 s  (9,044,008 bytes)
  ConstMap.from_bytes                     0.010 s
```

The single-lookup numbers are dominated by Python call overhead, not by the
map: use `get_many` when you have an array of keys to look up at once, and
`get_many_into` when you have somewhere to put the results.

To reproduce:

```
python benchmarks/benchmark.py 1000000
```

## How it works

Given *n* (key, value) pairs the algorithm:

1. Hashes each key with xxhash3 to a 64-bit value.
2. Maps each hashed key to three positions `h0, h1, h2` in an array of
   size ~1.125·*n*, using overlapping segments.
3. Finds, via [peeling][bff], an ordering in which each key has an
   exclusive cell among its three; walks that ordering in reverse,
   setting each cell so `array[h0] ^ array[h1] ^ array[h2] == value`.

Lookup is one xxhash, three array reads, and two XORs.

References:

> Thomas Mueller Graf and Daniel Lemire,
> [*Binary Fuse Filters: Fast and Smaller Than Xor Filters*][bff],
> ACM Journal of Experimental Algorithmics, Vol. 27, 2022.
> DOI: [10.1145/3510449](https://doi.org/10.1145/3510449)

## License

Apache License 2.0. See [LICENSE](LICENSE).

`fastconstmap` vendors xxHash, which is licensed under the BSD-2-clause
license; see `src/third_party/xxhash/LICENSE`.

[bff]:       https://arxiv.org/abs/2201.01174
[constmap]:  https://github.com/lemire/constmap
