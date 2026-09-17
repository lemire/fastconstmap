"""Interoperability with constmap (Go) and rsconstmap (Rust), and backward
compatibility with files fastconstmap 0.9 wrote.

The files under tests/interop were written from the same input as data():
constmap.* by the Go package (rsconstmap writes byte-identical files), and
fastconstmap-0.9.* by this package's 0.9 release, which hashed keys with XXH3
rather than the XXH64 all three implementations use now. A change to the
hash, the mixing, the seed sequence, the layout or the checksum shows up here
as a failure to load or a wrong value.
"""
import os

import pytest

from fastconstmap import ConstMap, PairedVerifiedConstMap, VerifiedConstMap

HERE = os.path.join(os.path.dirname(__file__), "interop")


def data():
    return {f"key-{i}": 7 * i for i in range(200)}


def path(name):
    return os.path.join(HERE, name)


def check(m, verified):
    for k, v in data().items():
        assert m[k] == v, k
    if verified:
        for i in range(1000):
            assert m.get(f"absent-{i}") is None


@pytest.mark.parametrize("Map, ext, verified", [
    (ConstMap, "cmap", False),
    (VerifiedConstMap, "vmap", True),
    (PairedVerifiedConstMap, "pmap", True),
])
def test_reads_go_and_rust_files(Map, ext, verified):
    m = Map.load(path(f"constmap.{ext}"))
    check(m, verified)
    # Go and Rust do not record the key count, so it is unknown here.
    assert len(m) == 0
    # A copy from bytes behaves the same.
    check(Map.from_bytes(open(path(f"constmap.{ext}"), "rb").read()), verified)


@pytest.mark.parametrize("Map, ext", [
    (VerifiedConstMap, "vmap"),
    (PairedVerifiedConstMap, "pmap"),
])
def test_zero_copy_view_of_go_file(Map, ext):
    """The 32-byte header of these two formats keeps the arrays 8-byte
    aligned, so a zero-copy view works on a Go file as on our own."""
    blob = open(path(f"constmap.{ext}"), "rb").read()
    check(Map.from_buffer(blob), True)


def test_go_constmap_file_cannot_be_viewed():
    """CMAP0001 has a 28-byte header, which leaves the array 4 bytes off an
    8-byte boundary in any aligned buffer, so from_buffer refuses it: use
    load() or from_bytes(), which copy."""
    blob = open(path("constmap.cmap"), "rb").read()
    with pytest.raises(ValueError, match="aligned"):
        ConstMap.from_buffer(blob)


@pytest.mark.parametrize("Map, ext", [
    (ConstMap, "cmap"),
    (VerifiedConstMap, "vmap"),
    (PairedVerifiedConstMap, "pmap"),
])
def test_writes_the_shared_format(Map, ext):
    """What this package writes is what Go and Rust write, except for the
    key count we keep in the header (which they ignore) and, for ConstMap,
    the magic that announces that extra field."""
    ours = Map(data()).to_bytes()
    theirs = open(path(f"constmap.{ext}"), "rb").read()
    if ext == "cmap":
        assert ours[:8] == b"CMAP0003" and theirs[:8] == b"CMAP0001"
        # Same header apart from the magic, then our 4-byte count, then the
        # same data (the checksums differ because the headers do).
        assert ours[8:28] == theirs[8:28]
        assert ours[32:-8] == theirs[28:-8]
    else:
        assert ours[:28] == theirs[:28]
        assert ours[32:-8] == theirs[32:-8]
    # And the map read back from our bytes is the same map.
    check(Map.from_bytes(ours), ext != "cmap")


@pytest.mark.parametrize("Map, ext", [
    (ConstMap, "cmap"),
    (VerifiedConstMap, "vmap"),
])
def test_reads_fastconstmap_0_9_files(Map, ext):
    """Files from 0.9 still load and answer correctly: the map keeps the
    XXH3 hash its file was built with, and keeps the legacy magic when
    re-saved, since its table cannot be converted to the shared format
    (Go and Rust would hash lookups with XXH64 against it)."""
    m = Map.load(path(f"fastconstmap-0.9.{ext}"))
    check(m, ext == "vmap")
    assert len(m) == 200
    check(Map.from_buffer(open(path(f"fastconstmap-0.9.{ext}"), "rb").read()), ext == "vmap")

    blob = m.to_bytes()
    assert blob[:8] == (b"CMAP0002" if ext == "cmap" else b"VCMP0002")
    check(Map.from_bytes(blob), ext == "vmap")

    # Rebuilding from the keys is what produces a shared-format file.
    rebuilt = Map({k: m[k] for k in data()})
    assert rebuilt.to_bytes()[:8] == (b"CMAP0003" if ext == "cmap" else b"VMAP0001")
