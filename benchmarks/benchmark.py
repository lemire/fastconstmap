"""Benchmark fastconstmap against the built-in dict.

Run as:
    python benchmarks/benchmark.py [N]

Defaults to N = 1,000,000 keys.
"""
from __future__ import annotations
import argparse
import random
import sys
import time
from typing import Callable

from fastconstmap import ConstMap, VerifiedConstMap


def make_data(n: int, seed: int = 0xC0FFEE) -> tuple[list[str], dict[str, int]]:
    rng = random.Random(seed)
    keys = [f"key-{i}-{rng.randint(0, 1 << 30):x}" for i in range(n)]
    return keys, {k: i for i, k in enumerate(keys)}


def time_loop(label: str, iterations: int, fn: Callable[[int], None]) -> float:
    # Warm-up so the JIT-equivalent (branch predictor, page faults) settles.
    for i in range(min(iterations, 1000)):
        fn(i)
    t0 = time.perf_counter()
    for i in range(iterations):
        fn(i)
    elapsed = time.perf_counter() - t0
    per_op_ns = elapsed * 1e9 / iterations
    print(f"  {label:<28} {per_op_ns:8.1f} ns/op  ({elapsed:.3f} s total)")
    return per_op_ns


def time_batch(label: str, batches: int, batch_size: int, fn: Callable[[], None]) -> float:
    for _ in range(min(batches, 5)):
        fn()
    t0 = time.perf_counter()
    for _ in range(batches):
        fn()
    elapsed = time.perf_counter() - t0
    per_op_ns = elapsed * 1e9 / (batches * batch_size)
    print(f"  {label:<28} {per_op_ns:8.1f} ns/op  "
          f"({elapsed*1000/batches:.2f} ms/batch of {batch_size})")
    return per_op_ns


def report_size(label: str, value: int, n: int) -> None:
    print(f"  {label:<28} {value:12,} bytes  ({value/n:.2f} bytes/key)")


def deep_sizeof(obj, seen: set[int] | None = None) -> int:
    """Recursive sizeof that walks dict / list / tuple / set, deduping by id.

    Returns the total bytes attributable to ``obj`` and everything it
    transitively references. Small interned ints (-5..256) and interned
    strings are counted once because of the id() dedup, which matches
    reality — they're shared with the rest of the process.
    """
    if seen is None:
        seen = set()
    obj_id = id(obj)
    if obj_id in seen:
        return 0
    seen.add(obj_id)
    size = sys.getsizeof(obj)
    if isinstance(obj, dict):
        for k, v in obj.items():
            size += deep_sizeof(k, seen)
            size += deep_sizeof(v, seen)
    elif isinstance(obj, (list, tuple, set, frozenset)):
        for item in obj:
            size += deep_sizeof(item, seen)
    return size


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("n", type=int, nargs="?", default=1_000_000)
    parser.add_argument("--lookups", type=int, default=2_000_000,
                        help="number of individual lookups to time")
    parser.add_argument("--batch-size", type=int, default=1024)
    parser.add_argument("--batches",    type=int, default=2000)
    args = parser.parse_args()
    n = args.n

    print(f"=== fastconstmap benchmark — n = {n:,} keys, "
          f"python {sys.version.split()[0]} ===\n")

    keys, d = make_data(n)
    # Lookup index pattern: shuffled to avoid trivial cache wins.
    rng = random.Random(1234)
    lookup_idx = [rng.randrange(n) for _ in range(args.lookups)]

    # ---- Construction time ----
    print("Construction:")
    t0 = time.perf_counter()
    cm = ConstMap(d)
    print(f"  ConstMap.__init__              {time.perf_counter()-t0:7.3f} s")

    t0 = time.perf_counter()
    vm = VerifiedConstMap(d)
    print(f"  VerifiedConstMap.__init__      {time.perf_counter()-t0:7.3f} s")

    t0 = time.perf_counter()
    pyd = dict(d)
    print(f"  dict(d)                        {time.perf_counter()-t0:7.3f} s")

    # ---- Memory ----
    # The ConstMap retains no string objects: cm.nbytes() is its entire
    # footprint. To compare honestly we walk the dict and add up the
    # dict-table size *plus* every str key and int value it references
    # (deduped by id, so interned objects count once).
    print("\nMemory:")
    report_size("ConstMap.nbytes",         cm.nbytes(), n)
    report_size("VerifiedConstMap.nbytes", vm.nbytes(), n)
    dict_total = deep_sizeof(pyd)
    report_size("dict (table+keys+values)", dict_total, n)
    print(f"  ratio dict / ConstMap          {dict_total / cm.nbytes():.1f}x")

    # ---- Single lookup ----
    print(f"\nSingle lookup, {args.lookups:,} ops:")

    time_loop("dict[k]",                     args.lookups, lambda i: pyd[keys[lookup_idx[i]]])
    time_loop("ConstMap[k]",                 args.lookups, lambda i: cm[keys[lookup_idx[i]]])
    time_loop("VerifiedConstMap[k]",         args.lookups, lambda i: vm[keys[lookup_idx[i]]])

    # ---- Batched lookup ----
    batch_keys = [keys[rng.randrange(n)] for _ in range(args.batch_size)]
    print(f"\nBatched lookup, {args.batches} × {args.batch_size}:")

    # dict batch: pure Python list comprehension is the natural comparison.
    time_batch("dict comprehension",         args.batches, args.batch_size,
               lambda: [pyd[k] for k in batch_keys])
    time_batch("ConstMap.get_many",          args.batches, args.batch_size,
               lambda: cm.get_many(batch_keys))
    time_batch("VerifiedConstMap.get_many",  args.batches, args.batch_size,
               lambda: vm.get_many(batch_keys))

    # ---- Serialization ----
    print("\nSerialization:")
    t0 = time.perf_counter()
    blob = cm.to_bytes()
    print(f"  ConstMap.to_bytes              {time.perf_counter()-t0:7.3f} s  ({len(blob):,} bytes)")
    t0 = time.perf_counter()
    cm2 = ConstMap.from_bytes(blob)
    print(f"  ConstMap.from_bytes            {time.perf_counter()-t0:7.3f} s")
    # Verify round-trip.
    assert cm2[keys[0]] == d[keys[0]]


if __name__ == "__main__":
    main()
