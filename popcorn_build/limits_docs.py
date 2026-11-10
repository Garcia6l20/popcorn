"""Read the limits blocks of the model sources and render one page per IP.

A limits block is a `/** */` doc block that holds a `@limits <id> [<Title>]` line, followed by
`@group <scopes> <kind> ["<heading>"]` sections of markdown. A quoted heading that is still open at
the end of its line continues over the next lines, joined by single spaces, so a formatter may wrap
it. Only doxygen tags of `DOXYGEN_TAGS` may appear before `@limits`, nothing but `@group` may follow
it up to the first group, and no tag may appear in a group body.

The groups of a page are ordered by the name of their source file, then by position in the file.
A page whose text must read in another order puts its blocks in files that sort that way.

The module needs the standard library only, so the support page generator and the unit tests run it
without importing popcorn.
"""

import difflib
import re
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

SCOPES = {"u5": "STM32U5", "h5": "STM32H5", "l1": "STM32L1", "l4": "STM32L4"}
KINDS = {
    "unmodelled": "Not modelled",
    "deviation": "Deviations",
    "rule": "Rules",
    "note": "Notes",
}
LIMITS_ID = re.compile(r"[a-z0-9_]+")

_BLOCK = re.compile(r"/\*\*.*?\*/", re.DOTALL)
_PREFIX = re.compile(r"^\s*\*? ?")
_TAG = re.compile(r"@(\w+)(?:\s+(.*))?$")
_GROUP = re.compile(r'(?P<scopes>[a-z0-9,]+)\s+(?P<kind>\w+)(?:\s+"(?P<heading>[^"]*)")?\s*$')
_REFERENCE = re.compile(r"\b(?:RM|ES|DS|PM|AN|UM)\d{4,5}\b")
_INFERRED = re.compile(r"\binferred\b", re.IGNORECASE)
_SENTENCE_END = re.compile(r"(?<!e\.g)(?<!i\.e)\.\s+(?=[A-Z`])")
_DOXYGEN_MARKUP = re.compile(r"[@\\][pc]\s+")
_OPENER = re.compile(r"@(?:limits|group)\b")
DOXYGEN_TAGS = frozenset(
    [
        "brief",
        "p",
        "c",
        "ref",
        "param",
        "tparam",
        "return",
        "returns",
        "throws",
        "note",
        "see",
        "file",
        "code",
        "endcode",
        "ingroup",
        "defgroup",
    ]
)


@dataclass(frozen=True, slots=True)
class Group:
    """One `@group` section.

    Attributes:
        scopes: The families it covers, empty for every family.
        kind: A key of `KINDS`.
        heading: The text of its `##` heading.
        body: The markdown under the heading.
        file: The source file of the block.
        line: The line of the `@group` tag in that file.
    """

    scopes: tuple[str, ...]
    kind: str
    heading: str
    body: str
    file: Path
    line: int

    @property
    def inferred(self) -> int:
        return len(_INFERRED.findall(self.body))

    @property
    def references(self) -> list[str]:
        return list(dict.fromkeys(_REFERENCE.findall(self.body)))


@dataclass(slots=True)
class Page:
    """The limits of one IP, merged from every block with its id."""

    id: str
    title: str = ""
    lead: str = ""
    sources: list[Path] = field(default_factory=list[Path])
    groups: list[Group] = field(default_factory=list[Group])

    @property
    def families(self) -> list[str]:
        scopes = {scope for group in self.groups for scope in (group.scopes or tuple(SCOPES))}
        return [name for key, name in SCOPES.items() if key in scopes]

    @property
    def inferred(self) -> int:
        return sum(group.inferred for group in self.groups)

    @property
    def references(self) -> list[str]:
        return list(dict.fromkeys(ref for group in self.groups for ref in group.references))


def _error(file: Path, line: int, what: str) -> ValueError:
    return ValueError(f"{file}:{line}: {what}")


def lead_sentence(lines: Sequence[str]) -> str:
    """The first sentence of the `@brief` paragraph in @p lines, empty without one."""
    text: list[str] = []
    for raw in lines:
        stripped = raw.strip()
        if not text:
            if stripped.startswith("@brief"):
                text.append(stripped.removeprefix("@brief").strip())
        elif not stripped or stripped.startswith("@"):
            break
        else:
            text.append(stripped)
    joined = _DOXYGEN_MARKUP.sub("", " ".join(part for part in text if part))
    if not joined:
        return ""
    sentence = _SENTENCE_END.split(joined, maxsplit=1)[0]
    return sentence if sentence.endswith(".") else sentence + "."


@dataclass(slots=True)
class _Open:
    line: int
    kind: str
    heading: str
    scopes: tuple[str, ...]
    body: list[str] = field(default_factory=list[str])


def _doc_lines(block: str) -> list[str]:
    """The lines of a `/** */` block without the comment markers and the ` * ` prefix."""
    inner = block.removeprefix("/**").removesuffix("*/")
    first, *rest = inner.split("\n")
    return [first.strip(), *(_PREFIX.sub("", line, count=1).rstrip() for line in rest)]


def _check_misspelled_tags(file: Path, first_line: int, lines: Sequence[str]) -> None:
    for offset, line in enumerate(lines):
        tag = _TAG.match(line)
        if tag is None or tag.group(1) in DOXYGEN_TAGS:
            continue
        if difflib.get_close_matches(tag.group(1).lower(), ("limits", "group"), n=1, cutoff=0.7):
            raise _error(file, first_line + offset, f"unknown tag @{tag.group(1)}")


def _parse_block(file: Path, first_line: int, block: str, pages: dict[str, Page]) -> None:
    lines = _doc_lines(block)
    if not any(_OPENER.match(line) for line in lines):
        _check_misspelled_tags(file, first_line, lines)
        return
    page: Page | None = None
    current: _Open | None = None
    before: list[str] = []

    def close() -> None:
        nonlocal current
        if current is None or page is None:
            return
        text = "\n".join(current.body).strip("\n")
        if not text.strip():
            raise _error(file, current.line, "@group has an empty body")
        if any(group.heading == current.heading for group in page.groups):
            raise _error(file, current.line, f"two groups of {page.id} share {current.heading!r}")
        page.groups.append(
            Group(current.scopes, current.kind, current.heading, text, file, current.line)
        )
        current = None

    index = 0
    while index < len(lines):
        line, at = lines[index], first_line + index
        index += 1
        tag = _TAG.match(line)
        word = tag.group(1) if tag else ""
        if tag and word == "limits":
            close()
            if page is not None:
                raise _error(file, at, "a block holds one @limits")
            page = _begin(file, at, (tag.group(2) or "").strip(), pages, before)
        elif tag and word == "group":
            if page is None:
                raise _error(file, at, "@group before @limits")
            close()
            text = (tag.group(2) or "").strip()
            while text.count('"') == 1:
                if index >= len(lines):
                    raise _error(file, at, "@group heading has no closing quote")
                text += " " + lines[index].strip()
                index += 1
            current = _begin_group(file, at, text)
        elif tag and current is not None:
            raise _error(file, at, f"tag @{word} inside a group body")
        elif tag and word not in DOXYGEN_TAGS:
            raise _error(file, at, f"unknown tag @{word}")
        elif current is not None:
            current.body.append(line)
        elif page is not None and line.strip():
            raise _error(file, at, "text between @limits and the first @group")
        elif page is None:
            before.append(line)
    close()


def _begin(file: Path, at: int, text: str, pages: dict[str, Page], before: list[str]) -> Page:
    ident, _, title = text.partition(" ")
    title = title.strip()
    if not LIMITS_ID.fullmatch(ident):
        raise _error(file, at, f"@limits id {ident!r} is not [a-z0-9_]+")
    page = pages.setdefault(ident, Page(ident))
    if file not in page.sources:
        page.sources.append(file)
    if title:
        if page.title:
            raise _error(file, at, f"@limits {ident} has a title already: {page.title!r}")
        page.title = title
        page.lead = lead_sentence(before)
    return page


def _begin_group(file: Path, at: int, text: str) -> _Open:
    match = _GROUP.fullmatch(text)
    if match is None:
        raise _error(file, at, f'@group needs <scopes> <kind> ["heading"], got {text!r}')
    kind = match["kind"]
    if kind not in KINDS:
        raise _error(file, at, f"unknown kind {kind!r}, expected one of {', '.join(KINDS)}")
    raw = match["scopes"].split(",")
    scopes: tuple[str, ...] = ()
    if raw != ["all"]:
        if len(set(raw)) != len(raw):
            raise _error(file, at, f"a scope repeats in {match['scopes']!r}")
        unknown = [scope for scope in raw if scope not in SCOPES]
        if unknown:
            expected = f"all or {', '.join(SCOPES)}"
            raise _error(file, at, f"unknown scope {unknown[0]!r}, expected {expected}")
        scopes = tuple(raw)
    heading = match["heading"]
    if heading is None:
        names = ", ".join(SCOPES[scope] for scope in scopes) if scopes else "Every family"
        heading = f"{names}: {KINDS[kind]}"
    return _Open(at, kind, heading, scopes)


def read_pages(roots: Iterable[Path]) -> dict[str, Page]:
    """The pages of every limits block in the `*.cpp` and `*.hpp` files directly under @p roots,
    files in name order and blocks in source order.

    @throws ValueError naming `file:line` for a malformed block, an id without a title, or two
        titles for one id.
    """
    files = sorted(
        (path for root in roots for pattern in ("*.cpp", "*.hpp") for path in root.glob(pattern)),
        key=lambda path: (path.name, str(path)),
    )
    pages: dict[str, Page] = {}
    for file in files:
        text = file.read_text()
        for match in _BLOCK.finditer(text):
            first = text.count("\n", 0, match.start()) + 1
            _parse_block(file, first, match.group(), pages)
    for page in pages.values():
        if not page.title:
            raise ValueError(f"{page.sources[0]}: @limits {page.id} has no title")
        page.sources.sort(key=lambda path: path.name)
    return pages


def render_page(page: Page, source_url: Callable[[Path], str]) -> str:
    """The markdown of one IP: title, sources, summary line and a section per group."""
    sources = ", ".join(f"[{path.name}]({source_url(path)})" for path in page.sources)
    summary = [", ".join(page.families) + "."]
    summary.append(f"{page.inferred} inferred.")
    if page.references:
        summary.append(f"References: {', '.join(page.references)}.")
    lines = [f"# {page.title}", ""]
    if page.lead:
        lines += [page.lead, ""]
    lines += [f"Sources: {sources}", "", " ".join(summary), ""]
    for group in page.groups:
        lines += [f"## {group.heading}", "", group.body, ""]
        if group.references:
            lines += [f"References: {', '.join(group.references)}.", ""]
    return "\n".join(lines)


def render_table(pages: Mapping[str, Page], prefix: str = "") -> str:
    """The table of every page: families, groups, inferred marks, references and sources, with a
    totals row. Links to the pages start with @p prefix."""
    rows = [
        "| IP | Families | Groups | inferred | References | Model sources |",
        "|---|---|---|---|---|---|",
    ]
    ordered = sorted(pages.values(), key=lambda page: page.title.lower())
    for page in ordered:
        rows.append(
            f"| [{page.title}]({prefix}{page.id}.md) | {', '.join(page.families)} "
            f"| {len(page.groups)} | {page.inferred} | {', '.join(page.references)} "
            f"| {', '.join(path.name for path in page.sources)} |"
        )
    groups = sum(len(page.groups) for page in ordered)
    inferred = sum(page.inferred for page in ordered)
    rows.append(f"| **{len(ordered)} IPs** | | {groups} | {inferred} | | |")
    return "\n".join(rows) + "\n"
