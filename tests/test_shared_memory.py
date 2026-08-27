"""Tests for the zero-copy / shared-memory API.

A ConstMap can be serialized into a buffer (e.g. a multiprocessing
SharedMemory block) and re-opened with `from_buffer` as a zero-copy view:
the map reads directly out of that buffer, so many processes can share one
copy of the data.
"""
import multiprocessing as mp
import os

import pytest

from fastconstmap import ConstMap, VerifiedConstMap


# --------------------------------------------------------------------------
# Same-process tests
# --------------------------------------------------------------------------

def test_serialized_size_matches_to_bytes():
    cm = ConstMap({f"k{i}": i for i in range(1000)})
    assert cm.serialized_size() == len(cm.to_bytes())


def test_write_into_then_from_buffer():
    d = {f"k{i}": i * 11 for i in range(2000)}
    cm = ConstMap(d)
    buf = bytearray(cm.serialized_size())
    written = cm.write_into(buf)
    assert written == cm.serialized_size()

    view = ConstMap.from_buffer(buf)
    for k, v in d.items():
        assert view[k] == v


def test_write_into_buffer_too_small():
    cm = ConstMap({"a": 1, "b": 2})
    too_small = bytearray(cm.serialized_size() - 1)
    with pytest.raises(ValueError):
        cm.write_into(too_small)


def test_from_buffer_is_zero_copy():
    # While a view is alive, the underlying buffer cannot be released —
    # proof that from_buffer holds a live view rather than copying.
    cm = ConstMap({f"k{i}": i for i in range(500)})
    mv = memoryview(bytearray(cm.to_bytes()))
    view = ConstMap.from_buffer(mv)
    with pytest.raises(BufferError):
        mv.release()
    del view
    mv.release()  # succeeds once the view is gone


def test_from_buffer_reflects_buffer_contents():
    # A view really reads through to the buffer: mutating the buffer's data
    # region changes what the view returns (don't do this in real code!).
    d = {"alpha": 111, "beta": 222}
    cm = ConstMap(d)
    buf = bytearray(cm.to_bytes())
    view = ConstMap.from_buffer(buf)
    assert view["alpha"] == 111
    # Each lookup XORs three data words; flipping every bit of the data
    # region (offset 32, length nbytes()) therefore flips every bit of every
    # result. This is deterministic and proves the view aliases the buffer.
    for i in range(32, 32 + cm.nbytes()):
        buf[i] ^= 0xFF
    mask = (1 << 64) - 1
    assert view["alpha"] == (~111 & mask)
    assert view["beta"] == (~222 & mask)
    del view


def test_from_buffer_on_bytes():
    d = {f"k{i}": i for i in range(300)}
    cm = ConstMap(d)
    view = ConstMap.from_buffer(cm.to_bytes())
    for k, v in d.items():
        assert view[k] == v


def test_from_buffer_unaligned_rejected():
    cm = ConstMap({"a": 1, "b": 2, "c": 3})
    blob = cm.to_bytes()
    # Offsetting by one byte makes the embedded uint64 array unaligned.
    padded = memoryview(bytearray(b"\x00" + blob))[1:]
    with pytest.raises(ValueError):
        ConstMap.from_buffer(padded)


def test_from_buffer_bad_magic():
    with pytest.raises(ValueError):
        ConstMap.from_buffer(bytearray(64))


def test_from_buffer_corrupted_checksum():
    cm = ConstMap({"a": 1, "b": 2, "c": 3})
    buf = bytearray(cm.to_bytes())
    buf[len(buf) // 2] ^= 0xFF
    with pytest.raises(ValueError):
        ConstMap.from_buffer(buf)


def test_verified_write_into_from_buffer():
    d = {f"k{i}": i for i in range(1500)}
    vm = VerifiedConstMap(d)
    buf = bytearray(vm.serialized_size())
    vm.write_into(buf)
    view = VerifiedConstMap.from_buffer(buf)
    for k, v in d.items():
        assert view[k] == v
    assert view.get("not-present") is None
    del view


# --------------------------------------------------------------------------
# Cross-process test using multiprocessing.shared_memory
# --------------------------------------------------------------------------

# Defined at module level so it is picklable under the "spawn" start method
# (the default on macOS and Windows).
def _shm_lookup_worker(shm_name, keys):
    """Open an existing SharedMemory block, view the map, look up `keys`."""
    from multiprocessing.shared_memory import SharedMemory

    from fastconstmap import ConstMap

    shm = SharedMemory(name=shm_name)
    cm = None
    try:
        cm = ConstMap.from_buffer(shm.buf)
        return [cm[k] for k in keys]
    finally:
        cm = None        # release the buffer view before closing the block
        shm.close()


def test_cross_process_shared_lookup():
    shared_memory = pytest.importorskip("multiprocessing.shared_memory")
    SharedMemory = shared_memory.SharedMemory

    n = 5000
    d = {f"key-{i}": i * 7 for i in range(n)}
    cm = ConstMap(d)

    try:
        shm = SharedMemory(create=True, size=cm.serialized_size())
    except OSError as exc:  # e.g. shared memory unavailable in the sandbox
        pytest.skip(f"shared memory unavailable: {exc}")

    try:
        cm.write_into(shm.buf)

        # Each worker looks up a different slice of the keys.
        key_list = list(d)
        batches = [key_list[i::4] for i in range(4)]

        ctx = mp.get_context("spawn")
        with ctx.Pool(processes=4) as pool:
            results = pool.starmap(
                _shm_lookup_worker,
                [(shm.name, batch) for batch in batches],
            )

        for batch, got in zip(batches, results):
            assert got == [d[k] for k in batch]
    finally:
        shm.close()
        try:
            shm.unlink()
        except FileNotFoundError:
            # A child's resource tracker may have unlinked it already.
            pass


def test_cross_process_named_shared_memory():
    """Producer picks an explicit name; consumers attach to that exact name."""
    shared_memory = pytest.importorskip("multiprocessing.shared_memory")
    SharedMemory = shared_memory.SharedMemory

    # An explicit, caller-chosen name (made unique so parallel test runs and
    # leftovers from a crashed run don't collide).
    shm_name = f"fastconstmap_test_{os.getpid()}"

    # Defensively clear any stale block left by a previous crashed run.
    try:
        SharedMemory(name=shm_name).unlink()
    except FileNotFoundError:
        pass

    n = 5000
    d = {f"key-{i}": i * 13 for i in range(n)}
    cm = ConstMap(d)

    try:
        shm = SharedMemory(create=True, size=cm.serialized_size(), name=shm_name)
    except OSError as exc:  # shared memory unavailable in the sandbox
        pytest.skip(f"shared memory unavailable: {exc}")

    # The created block must carry exactly the name we asked for.
    assert shm.name == shm_name

    try:
        cm.write_into(shm.buf)

        key_list = list(d)
        batches = [key_list[i::3] for i in range(3)]

        ctx = mp.get_context("spawn")
        with ctx.Pool(processes=3) as pool:
            # Workers receive the literal name string, not shm.name — proving
            # a consumer can hard-code / config the name with no auto value.
            results = pool.starmap(
                _shm_lookup_worker,
                [(shm_name, batch) for batch in batches],
            )

        for batch, got in zip(batches, results):
            assert got == [d[k] for k in batch]
    finally:
        shm.close()
        try:
            shm.unlink()
        except FileNotFoundError:
            pass
