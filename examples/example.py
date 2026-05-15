"""Tiny end-to-end example: build, look up, save, reload."""
from fastconstmap import ConstMap, VerifiedConstMap

d = {"apple": 100, "banana": 200, "cherry": 300}

# Minimal, no missing-key detection.
m = ConstMap(d)
print("apple   ->", m["apple"])
print("batched ->", m.get_many(["banana", "cherry"]))

# Dict-like, detects missing keys.
vm = VerifiedConstMap(d)
print("vm grape?", vm.get("grape"))
print("vm has banana?", "banana" in vm)

# Round-trip through bytes.
blob = m.to_bytes()
m2 = ConstMap.from_bytes(blob)
assert m2["banana"] == 200

# Round-trip through a file.
m.save("example.cmap")
m3 = ConstMap.load("example.cmap")
assert m3["cherry"] == 300
print("save/load OK")
