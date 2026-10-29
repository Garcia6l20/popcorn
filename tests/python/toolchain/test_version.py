"""The version derived from the git tag, the sdist `PKG-INFO` and the pretend variable."""

from __future__ import annotations

import subprocess
import tomllib
from pathlib import Path

import pytest

from popcorn_build import backend
from popcorn_build.version import PROJECT_DIR, popcorn_version

GIT_ENV = {
    "GIT_AUTHOR_NAME": "t",
    "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t",
    "GIT_COMMITTER_EMAIL": "t@example.invalid",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
}

PYPROJECT = (
    '[project]\nname = "popcorn"\ndynamic = ["version"]\n\n'
    '[tool.setuptools_scm]\nversion_scheme = "guess-next-dev"\nlocal_scheme = "no-local-version"\n'
    '\n[tool.setuptools_scm.tag]\nprefix = "v"\nstrict = true\n'
)


def git(root: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        capture_output=True,
        env={"PATH": "/usr/bin:/bin:/usr/local/bin"} | GIT_ENV,
    )


def commit(root: Path, name: str) -> None:
    (root / name).write_text(name)
    git(root, "add", name)
    git(root, "commit", "-q", "-m", name)


@pytest.fixture(autouse=True)
def no_pretend_version(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SETUPTOOLS_SCM_PRETEND_VERSION", raising=False)
    monkeypatch.delenv("SETUPTOOLS_SCM_PRETEND_VERSION_FOR_POPCORN", raising=False)


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    root.mkdir()
    git(root, "init", "-q", "-b", "main")
    (root / "pyproject.toml").write_text(PYPROJECT)
    git(root, "add", "pyproject.toml")
    git(root, "commit", "-q", "-m", "init")
    return root


def test_a_tagged_commit_gives_exactly_the_tag(repo: Path) -> None:
    git(repo, "tag", "v0.1.0")
    assert popcorn_version(repo) == "0.1.0"


def test_commits_after_the_tag_give_a_dev_version_with_no_local_segment(repo: Path) -> None:
    git(repo, "tag", "v0.1.0")
    commit(repo, "a")
    commit(repo, "b")
    assert popcorn_version(repo) == "0.1.1.dev2"


def test_untracked_files_leave_a_tagged_version_unchanged(repo: Path) -> None:
    git(repo, "tag", "v0.1.0")
    (repo / "untracked").write_text("x")
    assert popcorn_version(repo) == "0.1.0"


def test_a_modified_tracked_file_at_a_tag_gives_the_next_dev_version(repo: Path) -> None:
    git(repo, "tag", "v0.1.0")
    (repo / "pyproject.toml").write_text(PYPROJECT + "\n")
    assert popcorn_version(repo) == "0.1.1.dev0"


@pytest.mark.parametrize("tag", ["fold-r12-pre", "fold-tz-03-pre", "tz-10a-pre-move"])
def test_a_release_process_tag_at_head_does_not_set_the_version(repo: Path, tag: str) -> None:
    git(repo, "tag", "v0.1.0")
    commit(repo, "a")
    git(repo, "tag", tag)
    assert popcorn_version(repo) == "0.1.1.dev1"


def test_the_version_without_git_is_the_pkg_info_version(tmp_path: Path) -> None:
    (tmp_path / "pyproject.toml").write_text(PYPROJECT)
    (tmp_path / "PKG-INFO").write_text(
        "Metadata-Version: 2.4\nName: popcorn\nVersion: 3.4.5\n\nVersion: 9\n"
    )
    assert popcorn_version(tmp_path) == "3.4.5"


def test_the_pretend_variable_wins_over_pkg_info(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (tmp_path / "pyproject.toml").write_text(PYPROJECT)
    (tmp_path / "PKG-INFO").write_text("Metadata-Version: 2.4\nVersion: 3.4.5\n")
    monkeypatch.setenv("SETUPTOOLS_SCM_PRETEND_VERSION", "7.8.9")
    assert popcorn_version(tmp_path) == "7.8.9"


def test_the_backend_gives_pcons_the_computed_version(
    repo: Path, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setattr(backend, "PROJECT_DIR", repo.resolve())
    (repo / "pyproject.toml").write_text(PYPROJECT + '\n[tool.pcons]\nvariant = "release"\n')
    git(repo, "commit", "-q", "-a", "-m", "pcons")
    git(repo, "tag", "v2.3.4")
    monkeypatch.chdir(repo)
    info = backend.prepare_metadata_for_build_wheel(str(tmp_path))
    assert info == "popcorn-2.3.4.dist-info"
    assert "Version: 2.3.4\n" in (tmp_path / info / "METADATA").read_text()


def test_the_project_has_no_static_version() -> None:
    assert 'dynamic = ["version"]' in (PROJECT_DIR / "pyproject.toml").read_text()
    assert popcorn_version()


def test_the_backend_leaves_another_projects_static_version_alone(
    repo: Path, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    (repo / "pyproject.toml").write_text(
        '[project]\nname = "other"\nversion = "9.9.9"\n\n[tool.pcons]\nvariant = "release"\n'
    )
    git(repo, "commit", "-q", "-a", "-m", "other")
    git(repo, "tag", "v1.0.0")
    monkeypatch.chdir(repo)
    assert backend.prepare_metadata_for_build_wheel(str(tmp_path)) == "other-9.9.9.dist-info"


def test_the_project_counts_only_v_prefixed_tags() -> None:
    table = tomllib.loads((PROJECT_DIR / "pyproject.toml").read_text())["tool"]["setuptools_scm"]
    assert table["tag"] == {"prefix": "v", "strict": True}
