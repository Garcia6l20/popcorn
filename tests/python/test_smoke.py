import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "3c45ff7" in popcorn.version()
