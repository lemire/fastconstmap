"""Build configuration for the fastconstmap C extension."""
import platform

from setuptools import Extension, setup

extra_compile_args = ["-O3"]
extra_link_args = []

if platform.system() != "Windows":
    extra_compile_args += ["-std=c11"]
    # Math library for log/floor/round.
    extra_link_args += ["-lm"]
    # NB: do NOT pass -fvisibility=hidden. On Python 3.8 the PyMODINIT_FUNC
    # macro does not carry a visibility("default") attribute, so hidden
    # visibility would strip the PyInit__fastconstmap export and make the
    # extension unimportable.
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
