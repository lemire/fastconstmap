"""Concurrent use of the maps. On a free-threaded build (python3.14t) these
run truly in parallel; with the GIL they still exercise the same code."""
import array
import sys
import sysconfig
import threading

import pytest

from fastconstmap import NOT_FOUND, ConstMap, PairedVerifiedConstMap, VerifiedConstMap

N_THREADS = 8
TYPES = [ConstMap, VerifiedConstMap, PairedVerifiedConstMap]


def run_threads(target, n=N_THREADS):
    barrier = threading.Barrier(n)
    errors = []

    def wrapper(i):
        barrier.wait()
        try:
            target(i)
        except BaseException as e:  # pragma: no cover - reported below
            errors.append(e)

    threads = [threading.Thread(target=wrapper, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise errors[0]


def make_data(n=20_000):
    return {f"key-{i}": i * 7 for i in range(n)}


@pytest.mark.skipif(not sysconfig.get_config_var("Py_GIL_DISABLED"),
                    reason="needs a free-threaded build")
def test_gil_stays_disabled():
    # Importing the extension must not re-enable the GIL.
    assert not sys._is_gil_enabled()


@pytest.mark.parametrize("cls", TYPES)
def test_concurrent_lookups(cls):
    data = make_data()
    m = cls(data)
    keys = list(data)
    expected = [data[k] for k in keys]

    def work(i):
        for _ in range(3):
            assert m.get_many(keys) == expected
            out = array.array("Q", bytes(8 * len(keys)))
            assert m.get_many_into(keys, out) == len(keys)
            assert list(out) == expected
            for k in keys[i::N_THREADS]:
                assert m[k] == data[k]

    run_threads(work)


@pytest.mark.parametrize("cls", [VerifiedConstMap, PairedVerifiedConstMap])
def test_concurrent_absent_keys(cls):
    m = cls(make_data())
    absent = [f"absent-{i}" for i in range(5_000)]

    def work(i):
        assert m.get_many(absent) == [None] * len(absent)
        out = array.array("Q", bytes(8 * len(absent)))
        m.get_many_into(absent, out)
        assert set(out) == {NOT_FOUND}

    run_threads(work)


@pytest.mark.parametrize("cls", TYPES)
def test_concurrent_construction(cls):
    data = make_data(5_000)
    blob = cls(data).to_bytes()

    def work(i):
        local = {f"{i}-{k}": v for k, v in data.items()}
        m = cls(local)
        assert m.get_many(list(local)) == list(local.values())
        assert cls.from_bytes(blob).to_bytes() == blob

    run_threads(work)


@pytest.mark.parametrize("cls", TYPES)
def test_lookup_while_list_mutates(cls):
    # A key list shared with a thread that mutates it must not crash the
    # lookup; the results are whatever keys the list held at the time.
    data = make_data(2_000)
    m = cls(data)
    keys = list(data)
    stop = threading.Event()

    def mutate():
        while not stop.is_set():
            keys.append("x")
            keys.pop()
            keys.reverse()

    t = threading.Thread(target=mutate)
    t.start()
    try:
        for _ in range(200):
            assert len(m.get_many(keys)) in (len(data), len(data) + 1)
    finally:
        stop.set()
        t.join()


@pytest.mark.parametrize("cls", TYPES)
def test_construct_while_dict_mutates(cls):
    data = make_data(2_000)
    stop = threading.Event()

    def mutate():
        while not stop.is_set():
            data["extra"] = 1
            del data["extra"]

    t = threading.Thread(target=mutate)
    t.start()
    try:
        for _ in range(50):
            m = cls(data)
            assert m["key-0"] == 0
    finally:
        stop.set()
        t.join()


@pytest.mark.parametrize("cls", TYPES)
def test_init_cannot_be_called_twice(cls):
    m = cls({"a": 1})
    with pytest.raises(TypeError, match="immutable"):
        m.__init__({"b": 2})
    assert m["a"] == 1
    loaded = cls.from_bytes(m.to_bytes())
    with pytest.raises(TypeError, match="immutable"):
        loaded.__init__({"b": 2})
    assert loaded["a"] == 1
