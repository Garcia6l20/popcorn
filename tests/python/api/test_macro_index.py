"""The native macro index of an ELF file, `_core.read_macros`."""

from __future__ import annotations

import struct
from pathlib import Path

from popcorn import _core


def uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        low = value & 0x7F
        value >>= 7
        out.append(low | 0x80 if value else low)
        if not value:
            return bytes(out)


def elf_with_sections(sections: dict[str, bytes]) -> bytes:
    """A minimal ARM ELF32 executable holding `sections` and no program header."""
    names = b"\0" + b"".join(name.encode() + b"\0" for name in [*sections, ".shstrtab"])
    payload = bytearray()
    headers: list[tuple[int, int, int]] = []
    name_at = 1
    for name, data in [*sections.items(), (".shstrtab", names)]:
        headers.append((name_at, 52 + len(payload), len(data)))
        name_at += len(name) + 1
        payload += data
    payload += bytes(-len(payload) % 4)
    shoff = 52 + len(payload)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    header = ident + struct.pack(
        "<HHIIIIIHHHHHH", 2, 40, 1, 0, 0, shoff, 0, 52, 32, 0, 40, len(headers) + 1, len(headers)
    )
    table = bytes(40) + b"".join(
        struct.pack("<IIIIIIIIII", name, 1, 0, 0, offset, size, 0, 0, 1, 0)
        for name, offset, size in headers
    )
    return header + bytes(payload) + table


def test_a_macro_text_that_is_no_utf8_reads_with_surrogate_escapes(tmp_path: Path) -> None:
    abbrev = uleb(1) + uleb(0x11) + b"\0" + uleb(0x03) + uleb(0x08) + uleb(0x79) + uleb(0x17)
    abbrev += b"\0\0\0"
    die = uleb(1) + b"caf\xe9.c\0" + struct.pack("<I", 0)
    info = struct.pack("<IHBBI", 8 + len(die), 5, 1, 4, 0) + die
    macro = struct.pack("<HB", 5, 0) + b"\x01" + uleb(1) + b'LATIN "M\xfcller"\0' + b"\0"
    elf = tmp_path / "latin.elf"
    elf.write_bytes(
        elf_with_sections({".debug_info": info, ".debug_abbrev": abbrev, ".debug_macro": macro})
    )
    index = _core.read_macros(str(elf))
    assert index.units() == ["caf\udce9.c"]
    assert index.names() == ["LATIN"]
    [entry] = index.lookup("LATIN")
    assert entry.text == '"M\udcfcller"'
    assert entry.text.encode("utf-8", "surrogateescape") == b'"M\xfcller"'
