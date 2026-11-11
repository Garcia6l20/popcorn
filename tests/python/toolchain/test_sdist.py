"""The sdist ships the files git tracks, and a tree outside a git work tree ships what it holds."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from popcorn_build import backend

GIT_ENV = {
    "PATH": "/usr/bin:/bin:/usr/local/bin",
    "GIT_AUTHOR_NAME": "t",
    "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t",
    "GIT_COMMITTER_EMAIL": "t@example.invalid",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
}


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True, env=GIT_ENV)


def write(root: Path, name: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(name)


def listed(root: Path) -> set[str]:
    files = backend.tracked_sdist_files(root, root / "build/wheel")
    return {path.relative_to(root).as_posix() for path in files}


def test_the_sdist_lists_tracked_files_and_not_ignored_or_untracked_ones(tmp_path: Path) -> None:
    git(tmp_path, "init", "-q", "-b", "main")
    write(tmp_path, ".gitignore")
    (tmp_path / ".gitignore").write_text("local/\nbuild-*/\n*.pyi\n")
    for name in ("pyproject.toml", "python/popcorn/__init__.py", "native/src/a.cpp"):
        write(tmp_path, name)
    git(tmp_path, "add", ".")
    git(tmp_path, "commit", "-q", "-m", "init")
    for name in ("local/notes.md", "build-asan/x.o", "python/popcorn/_core.pyi", "scratch.txt"):
        write(tmp_path, name)
    assert listed(tmp_path) == {
        ".gitignore",
        "pyproject.toml",
        "python/popcorn/__init__.py",
        "native/src/a.cpp",
    }


def test_a_tracked_file_deleted_from_the_tree_is_not_listed(tmp_path: Path) -> None:
    git(tmp_path, "init", "-q", "-b", "main")
    write(tmp_path, "keep")
    write(tmp_path, "gone")
    git(tmp_path, "add", ".")
    git(tmp_path, "commit", "-q", "-m", "init")
    (tmp_path / "gone").unlink()
    assert listed(tmp_path) == {"keep"}


def test_a_tree_that_is_not_a_work_tree_ships_every_file(tmp_path: Path) -> None:
    write(tmp_path, "pyproject.toml")
    write(tmp_path, "native/a.cpp")
    assert listed(tmp_path) == {"pyproject.toml", "native/a.cpp"}


def test_a_directory_inside_another_work_tree_ships_every_file(tmp_path: Path) -> None:
    git(tmp_path, "init", "-q", "-b", "main")
    inner = tmp_path / "unpacked"
    write(inner, "pyproject.toml")
    write(inner, "native/a.cpp")
    assert listed(inner) == {"pyproject.toml", "native/a.cpp"}


def test_a_machine_with_no_git_ships_every_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    write(tmp_path, "pyproject.toml")
    empty = tmp_path / "empty"
    empty.mkdir()
    monkeypatch.setenv("PATH", str(empty))
    assert listed(tmp_path) == {"pyproject.toml"}
    assert "no git file list" in capsys.readouterr().err


def test_a_work_tree_with_submodules_not_checked_out_is_refused(tmp_path: Path) -> None:
    git(tmp_path, "init", "-q", "-b", "main")
    modules = "".join(
        f'[submodule "{name}"]\n\tpath = third_party/{name}\n\turl = https://example.invalid/{name}\n'
        for name in ("unicorn", "reflex")
    )
    (tmp_path / ".gitmodules").write_text(modules)
    write(tmp_path, "pyproject.toml")
    git(tmp_path, "add", ".")
    for path in ("third_party/unicorn", "third_party/reflex"):
        git(tmp_path, "update-index", "--add", "--cacheinfo", f"160000,{'1' * 40},{path}")
    git(tmp_path, "commit", "-q", "-m", "init")
    with pytest.raises(SystemExit, match="git submodule update --init"):
        listed(tmp_path)
