class DescriptionError(Exception):
    """A description source or patch is missing or malformed."""

    def __init__(self, file: str, line: int, key: str, expected: str) -> None:
        super().__init__(f"{file}:{line}: {key}: {expected}")
        self.file = file
        self.line = line
        self.key = key
        self.expected = expected
