"""Tiny end-to-end example: build, look up, save, reload."""
import array

from fastconstmap import NOT_FOUND, ConstMap, VerifiedConstMap

d = {"apple": 100, "banana": 200, "cherry": 300}

# Minimal, no missing-key detection.
m = ConstMap(d)
print("apple   ->", m["apple"])
print("batched ->", m.get_many(["banana", "cherry"]))

# Batched lookup straight into a buffer: no list, no Python int per key.
out = array.array("Q", [0]) * 3
m.get_many_into(["apple", "banana", "cherry"], out)
print("into buf ->", list(out))

# Dict-like, detects missing keys.
vm = VerifiedConstMap(d)
print("vm grape?", vm.get("grape"))
print("vm has banana?", "banana" in vm)
vm.get_many_into(["banana", "grape"], out)
print("vm batched ->", [None if v == NOT_FOUND else v for v in out[:2]])

# Round-trip through bytes.
blob = m.to_bytes()
m2 = ConstMap.from_bytes(blob)
assert m2["banana"] == 200

# Round-trip through a file.
m.save("example.cmap")
m3 = ConstMap.load("example.cmap")
assert m3["cherry"] == 300
print("save/load OK")
