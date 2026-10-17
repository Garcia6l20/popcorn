import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "048c13c" in popcorn.version()
