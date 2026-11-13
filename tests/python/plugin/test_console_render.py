"""The console block of the failure report: header, bounds, escaping, failed reads."""

from __future__ import annotations

import pytest
from popcorn.pytest._console import console_lines


def block(data: bytes, queued: int | None = None, dropped: int = 0, limit: int = 100) -> list[str]:
    total = len(data) if queued is None else queued
    return console_lines("X", lambda: (data, total, dropped), limit)


def test_the_header_names_the_cut_only_past_the_limit() -> None:
    assert block(b"abcd", limit=4)[0] == "console X (unread):"
    assert block(b"abcd", queued=5, limit=4)[0] == "console X (unread, last 4 of 5 bytes):"


def test_an_empty_queue_and_dropped_frames_are_said() -> None:
    assert block(b"") == ["console X (unread):", "  no bytes queued"]
    assert block(b"a", dropped=3) == ["console X (unread):", "  queue dropped 3 frames", "  a"]


def test_control_bytes_are_escaped_and_lines_split() -> None:
    assert block(b"a\xffb\x1b[31mc\r\n") == ["console X (unread):", "  a\\xffb\\x1b[31mc"]
    assert block(b"a\r\nb\rc")[1:] == ["  a", "  b", "  c"]
    assert block(b"a\tb")[1:] == ["  a\tb"]
    assert block(b"a\x0bb\x0cc")[1:] == ["  a\\x0bb\\x0cc"]


@pytest.mark.parametrize(
    ("data", "shown"),
    [
        ("\u009b".encode(), "\\x9b"),
        ("\u2028".encode(), "\\u2028"),
        (b"\x7f", "\\x7f"),
        ("\u202e".encode(), "\\u202e"),
        ("\U000e0001".encode(), "\\U000e0001"),
    ],
)
def test_unprintable_characters_are_escaped(data: bytes, shown: str) -> None:
    assert block(data)[1:] == [f"  {shown}"]


def test_printable_non_ascii_passes_through() -> None:
    assert block("caf\u00e9 \u20ac".encode())[1:] == ["  caf\u00e9 \u20ac"]


def test_a_cut_multibyte_character_shows_its_bytes() -> None:
    cut = "\u20ac".encode()[1:]
    assert block(cut)[1:] == ["  \\x82\\xac"]


def test_a_failed_read_is_one_line() -> None:
    def refused() -> tuple[bytes, int, int]:
        raise RuntimeError("X is bridged to /dev/pts/3")

    def broken() -> tuple[bytes, int, int]:
        raise ValueError("bad")

    assert console_lines("X", refused, 10) == [
        "console X: not readable: X is bridged to /dev/pts/3"
    ]
    assert console_lines("X", broken, 10) == ["console X: read failed: ValueError: bad"]
