import re
from pathlib import Path

import click
from popcorn.cli import cli

PAGE = Path(__file__).parents[2] / "docs/guide/cli.md"
NOT_OWNED = frozenset({"ai"})


def command_paths(group: click.Group, prefix: str = "popcorn") -> list[str]:
    paths: list[str] = []
    for name, command in sorted(group.commands.items()):
        if name in NOT_OWNED and prefix == "popcorn":
            continue
        path = f"{prefix} {name}"
        paths.append(path)
        if isinstance(command, click.Group):
            paths.extend(command_paths(command, path))
    return paths


def missing(text: str, paths: list[str]) -> list[str]:
    found: list[str] = []
    for path in paths:
        span = rf"`{re.escape(path)}(?=[ `])"
        heading = rf"^#+ .*{re.escape(path)}(?=[ `]|$)"
        if not re.search(span, text) and not re.search(heading, text, re.MULTILINE):
            found.append(path)
    return found


def test_every_command_path_of_the_group_is_on_the_page() -> None:
    paths = command_paths(cli)
    assert "popcorn profile show" in paths
    assert "popcorn part explain" in paths
    assert missing(PAGE.read_text(), paths) == []


def test_a_command_path_removed_from_the_page_is_reported() -> None:
    text = PAGE.read_text()
    paths = command_paths(cli)
    for path in paths:
        assert path in missing(text.replace(path, "popcorn removed"), paths)


def test_a_made_up_command_path_is_reported() -> None:
    assert missing(PAGE.read_text(), ["popcorn profile nothing"]) == ["popcorn profile nothing"]


def test_a_longer_command_name_does_not_stand_for_a_shorter_one() -> None:
    path = "popcorn profile show"
    assert missing("`popcorn profile showy`\n### `popcorn profile shown`", [path]) == [path]
    assert missing("`popcorn profile show` and more", [path]) == []
    assert missing("### `popcorn profile show`", [path]) == []


def option_rows(text: str) -> dict[str, str]:
    rows: dict[str, str] = {}
    for line in text.split("## Options", 1)[1].splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if line.startswith("|") and len(cells) == 3 and cells[0].startswith("`"):
            rows[cells[0].strip("`") + " " + cells[1]] = cells[2]
    return rows


def leaf_options(group: click.Group, prefix: str = "") -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    for name, command in sorted(group.commands.items()):
        if isinstance(command, click.Group):
            found.extend(leaf_options(command, f"{prefix}{name} "))
        else:
            for param in command.params:
                if isinstance(param, click.Option):
                    long = next(o for o in param.opts if o.startswith("--"))
                    if long != "--help":
                        found.append((f"{prefix}{name}", long))
    return found


def missing_options(text: str, options: list[tuple[str, str]]) -> list[tuple[str, str]]:
    rows = option_rows(text)
    return [
        (command, flag)
        for command, flag in options
        if not any(
            key.startswith(f"{command} ") and re.search(rf"`[^`]*{re.escape(flag)}\b", key)
            for key in rows
        )
    ]


def test_every_option_of_every_command_is_in_the_options_table() -> None:
    options = leaf_options(cli)
    assert ("profile diff", "--table") in options
    assert missing_options(PAGE.read_text(), options) == []


def test_an_option_missing_from_the_table_is_reported() -> None:
    options = leaf_options(cli)
    text = PAGE.read_text().replace("`--table NAME`", "`--other NAME`")
    assert ("profile diff", "--table") in missing_options(text, options)
    assert missing_options(PAGE.read_text(), [("profile diff", "--nothing")]) == [
        ("profile diff", "--nothing")
    ]
