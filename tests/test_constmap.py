"""Tests for fastconstmap."""
import os
import random
import tempfile

import pytest

from fastconstmap import ConstMap, VerifiedConstMap


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
