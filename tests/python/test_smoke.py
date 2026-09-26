import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "722da24" in popcorn.version()
