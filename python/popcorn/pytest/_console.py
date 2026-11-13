"""The console block of a failure report: the unread tail of a UART, rendered safe."""

from __future__ import annotations

from collections.abc import Callable


def console_lines(name: str, tail: Callable[[], tuple[bytes, int, int]], limit: int) -> list[str]:
    """The lines of the console block of UART `name`.

    `tail` returns the last bytes queued, the queued count and the dropped count. The block names
    the bytes shown out of those queued when `limit` cut them, says when nothing is queued or
    frames were dropped, then holds the text indented by two spaces. A read that fails gives one
    line naming `name` and the error, never an exception.
    """
    try:
        data, queued, dropped = tail()
        lines = _header(name, queued, limit, len(data), dropped)
        lines += [f"  {line}" for line in _text(data)]
    except RuntimeError as error:
        return [f"console {name}: not readable: {error}"]
    except Exception as error:
        return [f"console {name}: read failed: {type(error).__name__}: {error}"]
    return lines


def _header(name: str, queued: int, limit: int, shown: int, dropped: int) -> list[str]:
    if queued > limit:
        lines = [f"console {name} (unread, last {shown} of {queued} bytes):"]
    else:
        lines = [f"console {name} (unread):"]
    if queued == 0:
        lines.append("  no bytes queued")
    if dropped > 0:
        lines.append(f"  queue dropped {dropped} frames")
    return lines


def _text(data: bytes) -> list[str]:
    text = data.decode("utf-8", errors="backslashreplace")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = "".join(_escaped(char) for char in text)
    pieces = text.split("\n")
    if pieces and pieces[-1] == "":
        pieces.pop()
    return pieces


def _escaped(char: str) -> str:
    if char in "\n\t" or char.isprintable():
        return char
    code = ord(char)
    if code <= 0xFF:
        return f"\\x{code:02x}"
    if code <= 0xFFFF:
        return f"\\u{code:04x}"
    return f"\\U{code:08x}"
