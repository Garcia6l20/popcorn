from popcorn._errors import ConfigError


class DescriptionError(ConfigError):
    """A description source or patch is missing or malformed."""

    def __init__(self, file: str, line: int, key: str, expected: str) -> None:
        super().__init__(
            f"{file}:{line}: {key}: {expected}", file=file, line=line, key=key, expected=expected
        )
