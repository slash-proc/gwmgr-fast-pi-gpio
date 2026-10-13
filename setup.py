"""Setuptools entry point with an optional Cython supervision extension."""

from __future__ import annotations

from setuptools import find_packages, setup


ext_modules = []
try:
    from Cython.Build import cythonize
    from setuptools import Extension
except ImportError:
    pass
else:
    ext_modules = cythonize(
        [Extension("gnwmanager_fastpi._native", ["python/gnwmanager_fastpi/_native.pyx"])],
        compiler_directives={"language_level": "3"},
    )


setup(
    packages=find_packages("python"),
    package_dir={"": "python"},
    ext_modules=ext_modules,
)
