"""fastconstmap — fast, immutable map from strings to 64-bit integers.

Build a compact lookup structure from any ``dict[str, int]`` (or
``dict[bytes, int]``). Lookup costs one xxhash and three array reads.

Three classes are exposed:

* :class:`ConstMap` — minimal, ~9 bytes/key. Returns undefined values for
  keys that were not in the original mapping. Use when you know upstream
  that you will only look up keys you inserted.
* :class:`VerifiedConstMap` — ~18 bytes/key. Detects missing keys with
  probability ~1 − 2⁻⁶⁴. Behaves like a normal ``dict``: ``m[k]`` raises
  :class:`KeyError`, ``m.get(k, default)`` returns ``default``, ``k in m``
  works.
* :class:`PairedVerifiedConstMap` — same size and behaviour as
  :class:`VerifiedConstMap`, but stores each value next to its check word so
  a lookup of a present key touches three cache lines instead of six (and
  reads each as one 128-bit word on x86-64 and AArch64). Faster when most
  lookups hit; slower when most miss. Its serialised form is not
  interchangeable with :class:`VerifiedConstMap`'s.

All three are immutable after construction and can be serialised to bytes
or to a file path, in a format shared with the Go (``constmap``) and Rust
(``rsconstmap``) implementations: a file saved by any of the three loads in
the other two, on a little-endian host. Pass ``hash="xxh3"`` to a constructor
for a faster key hash whose files only fastconstmap reads. All also take batches: ``get_many`` returns a list, and
``get_many_into`` writes 64-bit words straight into a buffer you own (an
``array("Q")``, a numpy ``uint64`` array, a ``SharedMemory`` block), which
allocates nothing per key. A batch is faster than a loop because the map
hashes a block of keys before gathering any values, so the array reads of a
whole block overlap instead of being paid one after another. They can also be placed in a ``multiprocessing``
``SharedMemory`` block and opened by many processes with no per-process
copy — see ``serialized_size``, ``write_into`` and ``from_buffer``.
"""
from ._fastconstmap import ConstMap, VerifiedConstMap, PairedVerifiedConstMap, NOT_FOUND

__all__ = ["ConstMap", "VerifiedConstMap", "PairedVerifiedConstMap", "NOT_FOUND"]
__version__ = "0.10.0"
