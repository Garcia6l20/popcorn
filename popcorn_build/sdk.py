"""The plugin SDK digest and the generated `popcorn/plugin/build_info.hpp`.

The digest is the `;sdk` field of the plugin ABI tag. It covers the public headers under
native/include/popcorn and every reflex header they reach, so a plugin built against other
headers is refused.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

_INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)

_BLANKS = " \t\r\f\v"

_WHITESPACE = re.compile(r"[ \t\r\n\f\v]+")


def _collapse(text: str) -> str:
    return _WHITESPACE.sub(" ", text).strip(" ")


def _directive_aware(text: str) -> str:
    """@p text with each directive line kept on its own line and all other whitespace one space."""
    out: list[str] = []
    plain: list[str] = []
    directive: list[str] = []
    for line in text.split("\n"):
        if directive or line.lstrip(_BLANKS).startswith("#"):
            directive.append(line)
            if not line.rstrip(_BLANKS).endswith("\\"):
                if plain:
                    out.append(_collapse(" ".join(plain)))
                    plain = []
                out.append(_collapse(" ".join(directive)))
                directive = []
        else:
            plain.append(line)
    if directive:
        plain.extend(directive)
    if plain:
        out.append(_collapse(" ".join(plain)))
    return "\n".join(part for part in out if part)


def normalized_text(header: Path, compiler: str = "g++") -> str:
    """The text of @p header with comments removed and whitespace normalized.

    A directive line, with its `\\` continuations, keeps its ending newline. Every other run of
    ASCII whitespace, newlines included, is one space, so reflowed code keeps its text and a
    directive never merges with the line after it.

    @p compiler strips the comments, `-fpreprocessed -dD -E -P -x c++` keeps directives and
    expands no macro or include, so the text does not depend on include paths.
    @throws RuntimeError when @p compiler is missing, exits with an error or prints non-UTF-8.
    """
    command = [compiler, "-fpreprocessed", "-dD", "-E", "-P", "-x", "c++", str(header)]
    try:
        result = subprocess.run(command, capture_output=True, check=False)
    except OSError as error:
        raise RuntimeError(f"cannot run {compiler!r} to digest {header}: {error}") from error
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"{compiler!r} exited with {result.returncode} on {header}: {message}")
    try:
        output = result.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise RuntimeError(f"{compiler!r} printed non-UTF-8 text for {header}: {error}") from error
    return _directive_aware(output)


def _reflex_closure(roots: Sequence[Path], include_dirs: Sequence[Path]) -> set[Path]:
    """Every reflex header the files of @p roots include, directly or through reflex headers."""
    reached: set[Path] = set()
    pending = list(roots)
    while pending:
        source = pending.pop()
        for kind, name in _INCLUDE.findall(source.read_text()):
            candidates = [source.parent / name] if kind == '"' else []
            if name.startswith("reflex/"):
                candidates.extend(directory / name for directory in include_dirs)
            for candidate in candidates:
                if not candidate.is_file():
                    continue
                resolved = candidate.resolve()
                inside = any(resolved.is_relative_to(path.resolve()) for path in include_dirs)
                if inside and resolved not in reached:
                    reached.add(resolved)
                    pending.append(resolved)
                break
    return reached


def sdk_inputs(include_root: Path, reflex_include_dirs: Sequence[Path]) -> list[Path]:
    """Every file the digest reads: the public headers and the reflex headers they reach.

    The list is read when pcons configures the build, so a brand-new header joins the digest
    only at the next pcons run.
    """
    popcorn = sorted((include_root / "popcorn").rglob("*.hpp"))
    return [*popcorn, *sorted(_reflex_closure(popcorn, reflex_include_dirs))]


def reflex_include_dirs(reflex_root: Path) -> list[Path]:
    """The reflex include directories popcorn's public headers compile against."""
    return [reflex_root / component / "include" for component in ("core", "poly", "serde")]


def sdk_digest(
    include_root: Path,
    reflex_root: Path,
    reflex_include_dirs: Sequence[Path],
    compiler: str = "g++",
) -> str:
    """SHA-256, lower-case hex, over the SDK headers' paths and comment-free text, sorted by path.

    @p include_root is native/include, every `*.hpp` under its `popcorn` directory counts.
    @p reflex_include_dirs are the reflex include directories, @p reflex_root the directory
    reflex paths are recorded relative to. @p compiler strips the comments.
    """
    popcorn = sorted((include_root / "popcorn").rglob("*.hpp"))
    entries = [(f"popcorn:{path.relative_to(include_root).as_posix()}", path) for path in popcorn]
    entries.extend(
        (f"reflex:{path.relative_to(reflex_root.resolve()).as_posix()}", path)
        for path in _reflex_closure(popcorn, reflex_include_dirs)
    )
    digest = hashlib.sha256()
    for label, path in sorted(entries):
        content = normalized_text(path, compiler).encode()
        digest.update(f"{label}\0{len(content)}\0".encode())
        digest.update(content)
    return digest.hexdigest()


def build_info(version: str, nanobind_version: str, variant: str, digest: str) -> str:
    """The text of `popcorn/plugin/build_info.hpp`."""
    major, minor, patch = (int(part) for part in nanobind_version.split("."))
    return (
        "#pragma once\n\n"
        f'#define POPCORN_VERSION_STRING "{version}"\n'
        f'#define POPCORN_NANOBIND_VERSION "{nanobind_version}"\n'
        f"#define POPCORN_NANOBIND_VERSION_MAJOR {major}\n"
        f"#define POPCORN_NANOBIND_VERSION_MINOR {minor}\n"
        f"#define POPCORN_NANOBIND_VERSION_PATCH {patch}\n"
        '#define POPCORN_NB_DOMAIN ""\n'
        f'#define POPCORN_VARIANT "{variant}"\n'
        f'#define POPCORN_SDK_DIGEST "{digest}"\n'
    )


def main(argv: Sequence[str]) -> int:
    """Write build_info.hpp, the build edge behind it.

    Arguments: output, version, nanobind version, variant, native include root, reflex root and
    optionally the compiler, `g++` when absent.
    The file is rewritten only when its text changes, so its includers rebuild only then.
    """
    output, version, nanobind_version, variant, include_root, reflex_root, *rest = argv
    compiler = rest[0] if rest else "g++"
    root = Path(reflex_root)
    text = build_info(
        version,
        nanobind_version,
        variant,
        sdk_digest(Path(include_root), root, reflex_include_dirs(root), compiler),
    )
    target = Path(output)
    if not target.is_file() or target.read_text() != text:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
