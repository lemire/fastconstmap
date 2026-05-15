"""Build configuration for the fastconstmap C extension."""
import platform
import sys

from setuptools import Extension, setup

extra_compile_args = ["-O3"]
extra_link_args = []

if platform.system() != "Windows":
    extra_compile_args += ["-std=c11", "-fvisibility=hidden"]
    # Math library for log/floor/round.
    extra_link_args += ["-lm"]
    if sys.platform != "darwin":
        # On Linux we want native CPU features for xxhash; macOS uses universal2
        # builds for arm64+x86_64 so we leave -march to the compiler default.
        extra_compile_args += ["-fPIC"]
else:
    extra_compile_args += ["/O2"]

ext = Extension(
    name="fastconstmap._fastconstmap",
    sources=[
        "src/fastconstmap_module.c",
        "src/constmap.c",
    ],
    include_dirs=["src"],
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
    language="c",
)

setup(ext_modules=[ext])
