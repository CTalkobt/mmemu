#!/usr/bin/env python3
"""Setup configuration for mmemu-client Python package."""

from setuptools import setup, find_packages

with open("README.md", "r", encoding="utf-8") as fh:
    long_description = fh.read()

setup(
    name="mmemu-client",
    version="0.1.0",
    author="CTalkobt",
    author_email="ctalkobt@gmail.com",
    description="Python client library for MEGA65 emulator (mmemu) serial monitor interface",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/CTalkobt/mmemu",
    packages=find_packages(),
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Topic :: Software Development :: Debuggers",
        "Topic :: System :: Emulators",
    ],
    python_requires=">=3.8",
    install_requires=[],
    extras_require={
        "dev": ["pytest>=6.0", "black", "flake8", "mypy"],
    },
    entry_points={
        "console_scripts": [
            "mmemu-dump=mmemu_client.tools:memory_dump_main",
            "mmemu-disasm=mmemu_client.tools:disasm_main",
            "mmemu-vars=mmemu_client.tools:vars_main",
        ],
    },
)
