from pathlib import Path

from popcorn._errors import ConfigError, MissingInput


class DescriptionError(ConfigError):
    """A description source or patch is missing or malformed."""

    def __init__(self, file: str, line: int, key: str, expected: str) -> None:
        super().__init__(
            f"{file}:{line}: {key}: {expected}", file=file, line=line, key=key, expected=expected
        )


class MissingSource(MissingInput, DescriptionError):
    """A description source that does not exist: both a `MissingInput` and, for the callers
    that skip on it, a `DescriptionError`."""

    def __init__(self, file: str, key: str, expected: str, *, path: Path) -> None:
        DescriptionError.__init__(self, file, 0, key, expected)
        self._missing(path)
