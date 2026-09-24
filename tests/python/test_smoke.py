import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "938efd1" in popcorn.version()
