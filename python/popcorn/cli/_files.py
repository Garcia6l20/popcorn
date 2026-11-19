from collections.abc import Sequence
from pathlib import Path

from popcorn.profiling import Report


def expand(paths: Sequence[Path]) -> list[Path]:
    """The report files `paths` name: a file itself, a directory as its sorted `*.json` files,
    not recursive and without `.json.part`.

    Raises:
        FileNotFoundError: A path does not exist.
        ValueError: A directory holds no `*.json` file.
    """
    files: list[Path] = []
    for path in paths:
        if path.is_dir():
            found = sorted(path.glob("*.json"))
            if not found:
                raise ValueError(f"no *.json report in {path}")
            files.extend(found)
        elif path.exists():
            files.append(path)
        else:
            raise FileNotFoundError(f"{path}: no such file or directory")
    return files


def load_report(path: Path) -> Report:
    """The report in the JSON file `path`.

    Raises:
        ValueError: The file is no report, the message names the path.
    """
    try:
        return Report.from_json(path.read_text(encoding="utf-8"))
    except ValueError as exc:
        raise ValueError(f"{path}: {exc}") from exc
