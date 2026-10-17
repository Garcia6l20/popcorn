import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "923b308" in popcorn.version()
