"""Share one ConstMap across processes via multiprocessing.shared_memory.

The parent builds a ConstMap, writes it into a SharedMemory block, and
spawns workers that open the block as a zero-copy view — no process copies
the lookup data.

Run:  python examples/shared_memory_example.py
"""
import multiprocessing as mp
from multiprocessing.shared_memory import SharedMemory

from fastconstmap import ConstMap


def worker(shm_name, keys):
    """Attach to the shared block and look keys up — zero copy."""
    shm = SharedMemory(name=shm_name)
    cm = None
    try:
        cm = ConstMap.from_buffer(shm.buf)
        return [cm[k] for k in keys]
    finally:
        cm = None        # release the view before closing the block
        shm.close()


def main():
    data = {f"key-{i}": i * 7 for i in range(100_000)}
    cm = ConstMap(data)

    # Publish the map into shared memory.
    shm = SharedMemory(create=True, size=cm.serialized_size())
    try:
        cm.write_into(shm.buf)
        print(f"published {cm.serialized_size():,} bytes as '{shm.name}'")

        sample = ["key-1", "key-500", "key-99999"]
        ctx = mp.get_context("spawn")
        with ctx.Pool(processes=3) as pool:
            results = pool.starmap(
                worker, [(shm.name, sample)] * 3
            )

        for i, got in enumerate(results):
            assert got == [data[k] for k in sample]
            print(f"worker {i}: {sample} -> {got}")
        print("all workers agreed — one copy of the data, shared by all")
    finally:
        shm.close()
        try:
            shm.unlink()
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    main()
