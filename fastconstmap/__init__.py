"""fastconstmap — fast, immutable map from strings to 64-bit integers.

Build a compact lookup structure from any ``dict[str, int]`` (or
``dict[bytes, int]``). Lookup costs one xxhash and three array reads.

Two classes are exposed:

* :class:`ConstMap` — minimal, ~9 bytes/key. Returns undefined values for
  keys that were not in the original mapping. Use when you know upstream
  that you will only look up keys you inserted.
* :class:`VerifiedConstMap` — ~18 bytes/key. Detects missing keys with
  probability ~1 − 2⁻⁶⁴. Behaves like a normal ``dict``: ``m[k]`` raises
  :class:`KeyError`, ``m.get(k, default)`` returns ``default``, ``k in m``
  works.

Both types are immutable after construction and can be serialised to bytes
or to a file path. Both also take batches: ``get_many`` returns a list, and
``get_many_into`` writes 64-bit words straight into a buffer you own (an
``array("Q")``, a numpy ``uint64`` array, a ``SharedMemory`` block), which
allocates nothing per key. A batch is faster than a loop because the map
hashes a block of keys before gathering any values, so the array reads of a
whole block overlap instead of being paid one after another. They can also be placed in a ``multiprocessing``
``SharedMemory`` block and opened by many processes with no per-process
copy — see ``serialized_size``, ``write_into`` and ``from_buffer``.
"""
from ._fastconstmap import ConstMap, VerifiedConstMap, NOT_FOUND

__all__ = ["ConstMap", "VerifiedConstMap", "NOT_FOUND"]
__version__ = "0.9.0"
