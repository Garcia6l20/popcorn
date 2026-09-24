import popcorn
from popcorn._core import Engine


def test_version_names_unicorn_commit() -> None:
    assert "938efd1" in popcorn.version()


def test_engine_opens_and_closes() -> None:
    engine = Engine()
    assert engine.is_open()
    engine.close()
    assert not engine.is_open()
    engine.close()
