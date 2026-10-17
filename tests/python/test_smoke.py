import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "59e85d4" in popcorn.version()
