"""Cross-checks image::locate against addr2line on a real, uncommitted debug ELF.

Skipped unless `POPCORN_XCHECK_ELF` (the ELF to check) and `POPCORN_IMAGE_PROBE` (the
`image-probe` binary from step 05) are both set, and `arm-none-eabi-addr2line` is on PATH.
Runs in the normal gate too, where it always skips, so it stays typed and linted; it never
depends on a file outside the repository when it does.
"""

from __future__ import annotations

import os
import posixpath
import random
import shutil
import subprocess

import pytest

ELF_ENV = "POPCORN_XCHECK_ELF"
PROBE_ENV = "POPCORN_IMAGE_PROBE"

_elf = os.environ.get(ELF_ENV)
_probe = os.environ.get(PROBE_ENV)
_addr2line = shutil.which("arm-none-eabi-addr2line")

pytestmark = pytest.mark.skipif(
    _elf is None or _probe is None or _addr2line is None,
    reason=f"set {ELF_ENV} and {PROBE_ENV}, and put arm-none-eabi-addr2line on PATH",
)

SAMPLE_SIZE = 2000
SEED = 1234


def _run_probe(probe: str, elf: str, mode: str) -> list[str]:
    result = subprocess.run([probe, mode, elf], capture_output=True, text=True, check=True)
    return result.stdout.splitlines()


def _row_addresses(probe: str, elf: str) -> list[int]:
    addrs: list[int] = []
    for line in _run_probe(probe, elf, "rows"):
        addr_hex, line_no, _ = line.split(" ", 2)
        if int(line_no) != 0:
            addrs.append(int(addr_hex, 16))
    return addrs


def _symbol_halfwords(probe: str, elf: str) -> list[int]:
    addrs: list[int] = []
    for line in _run_probe(probe, elf, "symbols"):
        value_hex, size_text, kind, _ = line.split(" ", 3)
        size = int(size_text)
        if kind != "function" or size < 2:
            continue
        value = int(value_hex, 16)
        addrs.extend(range(value, value + size, 2))
    return addrs


def _locate(probe: str, elf: str, addresses: list[int]) -> list[str]:
    stdin_text = "\n".join(f"{addr:x}" for addr in addresses)
    result = subprocess.run(
        [probe, "locate", elf], input=stdin_text, capture_output=True, text=True, check=True
    )
    return result.stdout.splitlines()


def _addr2line_locate(addr2line: str, elf: str, addresses: list[int]) -> list[str]:
    stdin_text = "\n".join(f"{addr:#x}" for addr in addresses)
    result = subprocess.run(
        [addr2line, "-e", elf], input=stdin_text, capture_output=True, text=True, check=True
    )
    return [raw.split(" (discriminator", 1)[0].strip() for raw in result.stdout.splitlines()]


def test_xcheck_against_addr2line() -> None:
    assert _elf is not None
    assert _probe is not None
    assert _addr2line is not None

    rows = _row_addresses(_probe, _elf)
    row_sample = random.Random(SEED).sample(rows, min(SAMPLE_SIZE, len(rows)))

    halfwords = _symbol_halfwords(_probe, _elf)
    symbol_sample = random.Random(SEED).sample(halfwords, min(SAMPLE_SIZE, len(halfwords)))

    addresses = row_sample + symbol_sample
    ours = _locate(_probe, _elf, addresses)
    theirs = _addr2line_locate(_addr2line, _elf, addresses)

    compared = 0
    skipped = 0
    mismatches: list[str] = []
    for addr, our_line, their_line in zip(addresses, ours, theirs, strict=True):
        file_part, _, line_part = their_line.rpartition(":")
        if their_line in ("??:0", "??:?") or line_part in ("0", "?"):
            skipped += 1
            continue
        expected = f"{posixpath.normpath(file_part)}:{line_part}"
        compared += 1
        if our_line != expected:
            mismatches.append(f"{addr:#x}: ours={our_line!r} addr2line={expected!r}")

    print(
        f"sampled={len(addresses)} compared={compared} skipped={skipped} "
        f"mismatches={len(mismatches)}"
    )
    assert mismatches == []
