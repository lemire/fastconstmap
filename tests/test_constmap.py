"""Tests for fastconstmap."""
import array
import os
import random
import tempfile

import pytest

from fastconstmap import NOT_FOUND, ConstMap, VerifiedConstMap


# ----- ConstMap -----

def test_basic():
    d = {"apple": 100, "banana": 200, "cherry": 300, "date": 400, "elderberry": 500}
    m = ConstMap(d)
    for k, v in d.items():
        assert m[k] == v
        assert m.get(k) == v


def test_empty():
    m = ConstMap({})
    assert len(m) == 0


def test_len():
    d = {"apple": 100, "banana": 200, "cherry": 300}
    m = ConstMap(d)
    assert len(m) == 3


def test_len_after_serialization():
    d = {"a": 1, "b": 2, "c": 3, "d": 4}
    m = ConstMap(d)
    m2 = ConstMap.from_bytes(m.to_bytes())
    assert len(m2) == 4


def test_single_key():
    m = ConstMap({"only": 42})
    assert m["only"] == 42


def test_large():
    n = 100_000
    d = {f"key-{i}": i * 7 for i in range(n)}
    m = ConstMap(d)
    for k, v in d.items():
        assert m[k] == v


def test_bytes_keys():
    d = {b"alpha": 1, b"beta": 2}
    m = ConstMap(d)
    assert m[b"alpha"] == 1
    assert m[b"beta"] == 2


def test_get_many_list():
    d = {f"k{i}": i for i in range(20)}
    m = ConstMap(d)
    keys  = ["k5", "k0", "k19", "k7"]
    assert m.get_many(keys) == [5, 0, 19, 7]


def test_get_many_iterable():
    d = {f"k{i}": i for i in range(10)}
    m = ConstMap(d)
    assert m.get_many(iter(["k1", "k2", "k3"])) == [1, 2, 3]


def test_large_uint64_values():
    d = {"a": 2**64 - 1, "b": 2**63, "c": 0}
    m = ConstMap(d)
    assert m["a"] == 2**64 - 1
    assert m["b"] == 2**63
    assert m["c"] == 0


def test_signed_negative_value_reinterpreted():
    # -1 should round-trip through two's complement as 2**64-1.
    d = {"x": -1}
    m = ConstMap(d)
    assert m["x"] == 2**64 - 1


def test_rejects_non_string_key():
    with pytest.raises(TypeError):
        ConstMap({42: 1})


def test_rejects_non_int_value():
    with pytest.raises(TypeError):
        ConstMap({"a": "not int"})


def test_rejects_too_large_value():
    with pytest.raises(OverflowError):
        ConstMap({"a": 2**64})
    with pytest.raises(OverflowError):
        ConstMap({"a": -(2**63) - 1})


def test_random_values():
    rng = random.Random(42)
    n = 50_000
    d = {f"random-{i}-{rng.randint(0, 2**60)}": rng.getrandbits(64) for i in range(n)}
    m = ConstMap(d)
    for k, v in d.items():
        assert m[k] == v


# ----- ConstMap serialisation -----

def test_to_from_bytes():
    d = {"apple": 100, "banana": 200, "cherry": 300}
    m = ConstMap(d)
    b = m.to_bytes()
    m2 = ConstMap.from_bytes(b)
    for k, v in d.items():
        assert m2[k] == v


def test_from_bytes_corrupted():
    m = ConstMap({"a": 1, "b": 2, "c": 3})
    b = bytearray(m.to_bytes())
    b[len(b) // 2] ^= 0xFF
    with pytest.raises(ValueError):
        ConstMap.from_bytes(bytes(b))


def test_save_load(tmp_path):
    d = {f"k{i}": i for i in range(500)}
    m = ConstMap(d)
    p = tmp_path / "map.cmap"
    m.save(p)
    assert p.stat().st_size > 0
    m2 = ConstMap.load(p)
    for k, v in d.items():
        assert m2[k] == v


def test_from_bytes_bad_magic():
    with pytest.raises(ValueError):
        ConstMap.from_bytes(b"\x00" * 64)


def test_serialize_empty():
    m = ConstMap({})
    b = m.to_bytes()
    m2 = ConstMap.from_bytes(b)
    assert len(m2) == 0


# ----- VerifiedConstMap -----

def test_verified_basic():
    d = {"apple": 100, "banana": 200, "cherry": 300}
    vm = VerifiedConstMap(d)
    for k, v in d.items():
        assert vm[k] == v


def test_verified_len():
    d = {"apple": 100, "banana": 200, "cherry": 300}
    vm = VerifiedConstMap(d)
    assert len(vm) == 3


def test_verified_len_after_serialization():
    d = {"a": 1, "b": 2, "c": 3, "d": 4}
    vm = VerifiedConstMap(d)
    vm2 = VerifiedConstMap.from_bytes(vm.to_bytes())
    assert len(vm2) == 4


def test_verified_missing_raises():
    vm = VerifiedConstMap({"a": 1, "b": 2})
    with pytest.raises(KeyError):
        vm["nope"]


def test_verified_get_default():
    vm = VerifiedConstMap({"a": 1})
    assert vm.get("a") == 1
    assert vm.get("missing") is None
    assert vm.get("missing", -1) == -1


def test_verified_contains():
    vm = VerifiedConstMap({"a": 1, "b": 2})
    assert "a" in vm
    assert "missing" not in vm
    assert 42 not in vm  # non-string


def test_verified_get_many_with_missing():
    vm = VerifiedConstMap({"a": 1, "b": 2, "c": 3})
    out = vm.get_many(["a", "x", "c"], default=-1)
    assert out == [1, -1, 3]


def test_verified_large_with_misses():
    n = 50_000
    d = {f"k{i}": i for i in range(n)}
    vm = VerifiedConstMap(d)
    for k, v in d.items():
        assert vm[k] == v
    for i in range(1_000):
        assert vm.get(f"missing-{i}") is None


def test_verified_to_from_bytes():
    d = {"a": 1, "b": 2, "c": 3}
    vm = VerifiedConstMap(d)
    b = vm.to_bytes()
    vm2 = VerifiedConstMap.from_bytes(b)
    for k, v in d.items():
        assert vm2[k] == v
    assert "missing" not in vm2


def test_verified_save_load(tmp_path):
    d = {f"k{i}": i for i in range(200)}
    vm = VerifiedConstMap(d)
    p = tmp_path / "vmap.cmap"
    vm.save(p)
    vm2 = VerifiedConstMap.load(p)
    for k, v in d.items():
        assert vm2[k] == v
    assert vm2.get("not-there") is None


def test_constmap_and_verified_use_distinct_magic():
    cm = ConstMap({"a": 1})
    vm = VerifiedConstMap({"a": 1})
    with pytest.raises(ValueError):
        VerifiedConstMap.from_bytes(cm.to_bytes())
    with pytest.raises(ValueError):
        ConstMap.from_bytes(vm.to_bytes())


# ----- Batched lookups -----

def test_get_many_crosses_block_boundaries():
    # The core hashes a block of keys at a time; run a size that is not a
    # multiple of the block so the tail path is exercised too.
    for n in (1, 7, 8, 9, 63, 64, 65, 1000):
        d = {f"key{i}": i * 7 for i in range(n)}
        m = ConstMap(d)
        keys = list(d)
        assert m.get_many(keys) == [d[k] for k in keys]


def test_get_many_matches_single_lookup():
    rng = random.Random(99)
    d = {f"k{i}-{rng.random()}": rng.randrange(1 << 63) for i in range(5000)}
    m = ConstMap(d)
    keys = list(d)
    rng.shuffle(keys)
    assert m.get_many(keys) == [m[k] for k in keys]


def test_get_many_empty_and_mixed_types():
    d = {"a": 1, b"b": 2}
    m = ConstMap(d)
    assert m.get_many([]) == []
    assert m.get_many(["a", b"b"]) == [1, 2]


def test_get_many_rejects_non_string_key():
    m = ConstMap({"a": 1})
    with pytest.raises(TypeError):
        m.get_many(["a", 42])


def test_get_many_into():
    d = {f"key{i}": i * 3 for i in range(1000)}
    m = ConstMap(d)
    keys = list(d)
    out = array.array("Q", [0]) * len(keys)
    assert m.get_many_into(keys, out) == len(keys)
    assert list(out) == [d[k] for k in keys]


def test_get_many_into_unaligned_buffer():
    # A memoryview starting at an odd offset takes the staged path.
    d = {f"key{i}": i for i in range(100)}
    m = ConstMap(d)
    keys = list(d)
    raw = bytearray(8 * len(keys) + 8)
    view = memoryview(raw)[1:1 + 8 * len(keys)]
    assert m.get_many_into(keys, view) == len(keys)
    got = array.array("Q")
    got.frombytes(bytes(view))
    assert list(got) == [d[k] for k in keys]


def test_get_many_into_buffer_too_small():
    m = ConstMap({"a": 1, "b": 2})
    out = array.array("Q", [0])
    with pytest.raises(ValueError):
        m.get_many_into(["a", "b"], out)


def test_get_many_into_rejects_readonly_buffer():
    m = ConstMap({"a": 1})
    with pytest.raises((TypeError, BufferError)):
        m.get_many_into(["a"], b"\x00" * 8)


def test_verified_get_many_crosses_block_boundaries():
    for n in (1, 7, 8, 9, 63, 64, 65, 1000):
        d = {f"key{i}": i * 7 for i in range(n)}
        vm = VerifiedConstMap(d)
        keys = list(d) + ["missing-key", "another-missing"]
        assert vm.get_many(keys, default=-1) == [d[k] for k in d] + [-1, -1]


def test_verified_get_many_matches_single_lookup():
    rng = random.Random(4242)
    d = {f"k{i}-{rng.random()}": rng.randrange(1 << 63) for i in range(5000)}
    vm = VerifiedConstMap(d)
    keys = list(d)[:2500] + [f"absent-{i}" for i in range(2500)]
    rng.shuffle(keys)
    assert vm.get_many(keys) == [vm.get(k) for k in keys]


def test_verified_get_many_into_marks_missing():
    d = {f"key{i}": i * 5 for i in range(500)}
    vm = VerifiedConstMap(d)
    keys = list(d) + ["nope", "still-nope"]
    out = array.array("Q", [0]) * len(keys)
    assert vm.get_many_into(keys, out) == len(keys)
    assert list(out[:500]) == [d[k] for k in list(d)]
    assert out[500] == NOT_FOUND
    assert out[501] == NOT_FOUND


def test_get_many_into_empty_map():
    out = array.array("Q", [0]) * 4
    ConstMap({}).get_many_into(["a", "b", "c", "d"], out)
    assert list(out) == [0, 0, 0, 0]
    VerifiedConstMap({}).get_many_into(["a", "b", "c", "d"], out)
    assert list(out) == [NOT_FOUND] * 4


def test_not_found_sentinel():
    assert NOT_FOUND == 2 ** 64 - 1
