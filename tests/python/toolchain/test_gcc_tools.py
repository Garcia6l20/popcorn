import os
from pathlib import Path

import pytest

from popcorn_build.gcc_tools import gcc_ar_for


@pytest.mark.parametrize(
    ("compiler", "archiver"),
    [
        ("g++", "gcc-ar"),
        ("g++-16", "gcc-ar-16"),
        ("x86_64-pc-linux-gnu-g++", "x86_64-pc-linux-gnu-gcc-ar"),
        ("x86_64-pc-linux-gnu-g++-16", "x86_64-pc-linux-gnu-gcc-ar-16"),
    ],
)
def test_gcc_ar_is_the_sibling_with_the_same_prefix_and_suffix(
    tmp_path: Path, compiler: str, archiver: str
) -> None:
    (tmp_path / compiler).touch()
    (tmp_path / archiver).touch()
    assert gcc_ar_for(tmp_path / compiler, search_path="") == tmp_path / archiver


def test_a_wrapper_directory_holding_only_gxx_uses_the_next_compiler_on_the_path(
    tmp_path: Path,
) -> None:
    wrapper = tmp_path / "ccache" / "bin"
    real = tmp_path / "usr" / "bin"
    earlier = tmp_path / "earlier"
    for directory in (wrapper, real, earlier):
        directory.mkdir(parents=True)
    (tmp_path / "ccache-binary").touch()
    (wrapper / "g++").symlink_to(tmp_path / "ccache-binary")
    (earlier / "g++").touch()
    (earlier / "gcc-ar").touch()
    (real / "g++").touch()
    (real / "gcc-ar").touch()
    search_path = os.pathsep.join(str(entry) for entry in (earlier, wrapper, real))
    assert gcc_ar_for(wrapper / "g++", search_path=search_path) == real / "gcc-ar"


def test_a_symlinked_compiler_uses_the_gcc_ar_of_its_target(tmp_path: Path) -> None:
    link = tmp_path / "local" / "bin"
    prefix = tmp_path / "opt" / "gcc" / "bin"
    link.mkdir(parents=True)
    prefix.mkdir(parents=True)
    (prefix / "g++-16").touch()
    (prefix / "gcc-ar-16").touch()
    (link / "g++").symlink_to(prefix / "g++-16")
    assert gcc_ar_for(link / "g++", search_path="") == prefix / "gcc-ar-16"


def test_a_missing_gcc_ar_names_the_expected_path(tmp_path: Path) -> None:
    (tmp_path / "g++-16").touch()
    with pytest.raises(SystemExit, match="gcc-ar-16"):
        gcc_ar_for(tmp_path / "g++-16", search_path="")


def test_a_compiler_name_without_gxx_is_refused(tmp_path: Path) -> None:
    (tmp_path / "c++").touch()
    with pytest.raises(SystemExit, match="no g\\+\\+"):
        gcc_ar_for(tmp_path / "c++", search_path="")
