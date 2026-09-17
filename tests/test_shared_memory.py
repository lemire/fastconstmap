"""Tests for the zero-copy / shared-memory API.

A ConstMap can be serialized into a buffer (e.g. a multiprocessing
SharedMemory block) and re-opened with `from_buffer` as a zero-copy view:
the map reads directly out of that buffer, so many processes can share one
copy of the data.
"""
import multiprocessing as mp
import os

import pytest

from fastconstmap import ConstMap, PairedVerifiedConstMap, VerifiedConstMap


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


@pytest.mark.parametrize("Map", [ConstMap, VerifiedConstMap, PairedVerifiedConstMap])
def test_from_buffer_unaligned_rejected(Map):
    m = Map({"a": 1, "b": 2, "c": 3})
    blob = m.to_bytes()
    # Offsetting by one byte makes the embedded uint64 array unaligned.
    padded = memoryview(bytearray(b"\x00" + blob))[1:]
    with pytest.raises(ValueError):
        Map.from_buffer(padded)


def test_paired_from_buffer_eight_byte_aligned():
    """The zero-copy contract is 8-byte alignment. A paired slot is 16 bytes
    and is read with unaligned vector loads, so a buffer that is 8-byte but
    not 16-byte aligned must work, just with each slot possibly straddling
    two cache lines."""
    import ctypes

    d = {f"k{i}": i for i in range(3000)}
    pm = PairedVerifiedConstMap(d)
    blob = pm.to_bytes()
    backing = bytearray(16 + len(blob))
    # Place the payload at the offset within the first 16 bytes at which its
    # address is 8 mod 16, using the buffer's real address.
    addr = ctypes.addressof(ctypes.c_char.from_buffer(backing))
    off = next(o for o in range(16) if (addr + o) % 16 == 8)
    backing[off:off + len(blob)] = blob
    view = PairedVerifiedConstMap.from_buffer(memoryview(backing)[off:off + len(blob)])
    for k, v in d.items():
        assert view[k] == v
    assert view.get("absent") is None
    assert view.get_many(list(d)[:100]) == list(range(100))
    del view


def test_paired_from_bytes_rejects_inconsistent_parameters():
    """A checksum-valid paired file whose segment parameters do not describe
    its slot count is refused: lookups index the slots without bounds checks
    and trust those parameters."""
    import struct
    pm = PairedVerifiedConstMap({f"k{i}": i for i in range(1000)})
    blob = bytearray(pm.to_bytes())
    seg_len, seg_count, slots = struct.unpack_from("<III", blob, 16)
    assert (seg_count + 2) * seg_len == slots

    def with_header(seg_len, seg_count):
        bad = bytearray(blob)
        struct.pack_into("<II", bad, 16, seg_len, seg_count)
        # Recompute the FNV-1a trailer so only the parameter check can fire.
        h = 0xCBF29CE484222325
        for b in bad[:-8]:
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        struct.pack_into("<Q", bad, len(bad) - 8, h)
        return bytes(bad)

    # The intact bytes round-trip, so the rewrite itself is sound.
    assert PairedVerifiedConstMap.from_bytes(with_header(seg_len, seg_count))["k7"] == 7
    for bad_len, bad_count in [(seg_len, 2 * seg_count), (3, seg_count), (slots // 2, 0)]:
        with pytest.raises(ValueError, match="segment parameters"):
            PairedVerifiedConstMap.from_bytes(with_header(bad_len, bad_count))
        with pytest.raises(ValueError, match="segment parameters"):
            PairedVerifiedConstMap.from_buffer(with_header(bad_len, bad_count))


def test_from_buffer_bad_magic():
    with pytest.raises(ValueError):
        ConstMap.from_buffer(bytearray(64))


def test_from_buffer_corrupted_checksum():
    cm = ConstMap({"a": 1, "b": 2, "c": 3})
    buf = bytearray(cm.to_bytes())
    buf[len(buf) // 2] ^= 0xFF
    with pytest.raises(ValueError):
        ConstMap.from_buffer(buf)


@pytest.mark.parametrize("Verified", [VerifiedConstMap, PairedVerifiedConstMap])
def test_verified_write_into_from_buffer(Verified):
    d = {f"k{i}": i for i in range(1500)}
    vm = Verified(d)
    buf = bytearray(vm.serialized_size())
    vm.write_into(buf)
    view = Verified.from_buffer(buf)
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
