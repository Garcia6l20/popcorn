"""popcorn.coverage.report specs: `kind[:target]` parsing and the lcov report of `generate`."""

from __future__ import annotations

import json
import shutil
import stat
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

import pytest
from popcorn.coverage.report import ReportSpec, generate, parse_lcov, parse_spec

PREBUILT = Path(__file__).parents[2] / "firmware" / "obs" / "prebuilt"
TRACEFILE = (
    "TN:\nSF:src/a.c\nFN:1,3,f\nFNDA:2,f\nFNF:1\nFNH:1\nDA:1,2\nDA:2,0\nLF:2\nLH:1\n"
    "end_of_record\nTN:\nSF:src/b.c\nDA:1,1\nLF:1\nLH:1\nend_of_record\n"
)


def test_parse_spec_defaults_and_targets() -> None:
    base = Path("/base")
    assert parse_spec("term") == ReportSpec("term")
    assert parse_spec("html") == ReportSpec("html", Path("htmlcov"))
    assert parse_spec("xml", base) == ReportSpec("xml", base / "coverage.xml")
    assert parse_spec("lcov") == ReportSpec("lcov")
    assert parse_spec("lcov:out/a.info", base) == ReportSpec("lcov", base / "out" / "a.info")
    assert parse_spec("html:/abs/dir", base) == ReportSpec("html", Path("/abs/dir"))


@pytest.mark.parametrize("text", ["json", "term:x", "xml:", "html:", "lcov:", ":x", ""])
def test_parse_spec_refuses(text: str) -> None:
    with pytest.raises(ValueError):
        parse_spec(text)


def test_generate_lcov_without_filter_keeps_the_bytes(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "deep" / "out.info"
    result = generate(source, [ReportSpec("lcov", out)], root=tmp_path)
    assert out.read_text() == TRACEFILE
    assert result.written == (out,)
    assert result.totals.lines.found == 3


def test_generate_lcov_filters_and_counts_the_kept_files(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out.info"
    result = generate(source, [ReportSpec("lcov", out)], omit=["**/b.c"], root=tmp_path)
    kept = parse_lcov(out.read_text())
    assert [file.path for file in kept.files] == ["src/a.c"]
    assert result.totals.lines.found == 2


def test_generate_refuses_what_it_cannot_write(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    with pytest.raises(ValueError):
        generate(source, [ReportSpec("lcov")], root=tmp_path)
    with pytest.raises(ValueError):
        generate(source, [ReportSpec("xml")], root=tmp_path)


def test_generate_returns_the_term_text_and_writes_nothing(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    result = generate(source, [ReportSpec("term")], omit=["**/b.c"], root=tmp_path)
    assert result.written == ()
    assert result.term is not None
    assert "src/a.c" in result.term
    assert "src/b.c" not in result.term
    assert generate(source, [], root=tmp_path).term is None


def test_generate_checks_every_spec_before_writing(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out.info"
    with pytest.raises(ValueError):
        generate(source, [ReportSpec("lcov", out), ReportSpec("lcov")], root=tmp_path)
    assert not out.exists()


def test_generate_copies_bytes_whatever_the_locale(tmp_path: Path) -> None:
    text = TRACEFILE.replace("src/a.c", "src/\u00e9.c")
    source = tmp_path / "in.info"
    raw = text.replace("\n", "\r\n").encode()
    source.write_bytes(raw)
    out = tmp_path / "out.info"
    generate(source, [ReportSpec("lcov", out)], root=tmp_path)
    assert out.read_bytes() == raw
    generate(source, [ReportSpec("lcov", out)], omit=["**/b.c"], root=tmp_path)
    assert "src/\u00e9.c" in out.read_bytes().decode()


RECORDER = """#!{python}
import json, os, sys, time
from pathlib import Path

args = sys.argv[1:]
Path({log!r}).write_text(
    json.dumps({{"argv": args, "cwd": os.getcwd(), "lcov": Path(args[-1]).read_text()}})
)
out = Path(args[args.index("-o") + 1])
out.mkdir(parents=True, exist_ok=True)
(out / "partial.txt").write_text("partial")
if {index}:
    (out / "index.html").write_text("fake")
    (out / "gcov.css").write_text("css")
time.sleep({sleep})
sys.stderr.write({stderr!r})
sys.exit({code})
"""


def fake_genhtml(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    code: int = 0,
    *,
    index: bool = True,
    sleep: float = 0,
) -> Path:
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    log = tmp_path / "call.json"
    script = bin_dir / "genhtml"
    script.write_text(
        RECORDER.format(
            python=sys.executable,
            log=str(log),
            stderr="boom\n",
            code=code,
            index=index,
            sleep=sleep,
        )
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("PATH", str(bin_dir))
    return log


def test_html_runs_genhtml_from_the_root_without_a_shell(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    log = fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    root = tmp_path / "root dir"
    root.mkdir()
    out = tmp_path / "out dir;touch pwned"
    result = generate(source, [ReportSpec("html", out)], root=root)
    call = json.loads(log.read_text())
    assert result.written == (out,)
    assert result.problems == ()
    assert call["cwd"] == str(root)
    assert call["argv"][call["argv"].index("-o") + 1] == str(out)
    assert call["argv"][-1] == str(source)
    assert "--branch-coverage" in call["argv"]
    assert "--function-coverage" in call["argv"]
    assert "--synthesize-missing" in call["argv"]
    ignored = call["argv"][call["argv"].index("--ignore-errors") + 1].split(",")
    assert sorted(ignored) == ["inconsistent", "source"]
    assert not (root / "pwned").exists()
    assert not (tmp_path / "pwned").exists()


def test_html_gives_genhtml_the_filtered_tracefile(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    log = fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    generate(source, [ReportSpec("html", tmp_path / "out")], omit=["**/b.c"], root=tmp_path)
    call = json.loads(log.read_text())
    assert call["argv"][-1] != str(source)
    assert [file.path for file in parse_lcov(call["lcov"]).files] == ["src/a.c"]


def test_html_reports_a_failed_genhtml_and_writes_the_other_reports(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch, code=3)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "copy.info"
    result = generate(
        source, [ReportSpec("html", tmp_path / "out"), ReportSpec("lcov", out)], root=tmp_path
    )
    assert result.written == (out,)
    assert result.problems == ("html report not written, genhtml exited 3: boom",)
    assert out.read_text() == TRACEFILE


def test_html_empties_a_directory_holding_an_index_and_keeps_a_foreign_one(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    stale = tmp_path / "stale"
    stale.mkdir()
    (stale / "index.html").write_text("old")
    (stale / "gcov.css").write_text("old")
    (stale / "old.css").write_text("old")
    result = generate(source, [ReportSpec("html", stale)], root=tmp_path)
    assert result.problems == ()
    assert (stale / "index.html").read_text() == "fake"
    assert not (stale / "old.css").exists()
    foreign = tmp_path / "foreign"
    foreign.mkdir()
    (foreign / "notes.txt").write_text("mine")
    result = generate(source, [ReportSpec("html", foreign)], root=tmp_path)
    assert result.written == ()
    assert result.problems == (
        f"html report not written, {foreign} is not empty and holds no earlier html report",
    )
    assert [path.name for path in foreign.iterdir()] == ["notes.txt"]
    empty = tmp_path / "empty"
    empty.mkdir()
    assert generate(source, [ReportSpec("html", empty)], root=tmp_path).written == (empty,)
    blocked = tmp_path / "blocked"
    blocked.write_text("a file")
    result = generate(source, [ReportSpec("html", blocked)], root=tmp_path)
    assert result.problems == (f"html report not written, {blocked} is not a directory",)
    assert blocked.read_text() == "a file"


@pytest.mark.skipif(shutil.which("genhtml") is None, reason="genhtml is not installed")
def test_html_renders_a_tracefile_whose_source_is_missing(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE.replace("src/a.c", "/nonexistent/src/a.c"))
    out = tmp_path / "out"
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.problems == ()
    assert (out / "index.html").is_file()


def test_html_removes_what_a_failed_genhtml_left_so_a_rerun_works(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch, code=1, index=False)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.problems == ("html report not written, genhtml exited 1: boom",)
    assert not out.exists()
    (tmp_path / "again").mkdir()
    fake_genhtml(tmp_path / "again", monkeypatch)
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.problems == ()
    assert (out / "index.html").is_file()


def test_html_removes_an_emptied_report_when_genhtml_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch, code=1, index=False)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    out.mkdir()
    (out / "index.html").write_text("old")
    (out / "gcov.css").write_text("old")
    generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert not out.exists()


def test_html_kills_a_genhtml_that_runs_past_the_timeout(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch, sleep=30)
    monkeypatch.setattr("popcorn.coverage.report._genhtml.GENHTML_TIMEOUT", 0.5)
    monkeypatch.setattr("popcorn.coverage.report._spec.GENHTML_TIMEOUT", 0.5)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.written == ()
    assert result.problems == ("html report not written, genhtml ran past 0.5 s",)
    assert not out.exists()


def test_html_keeps_a_handwritten_site(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    site = tmp_path / "docs"
    site.mkdir()
    (site / "index.html").write_text("mine")
    (site / "page.md").write_text("mine")
    (site / "style.css").write_text("mine")
    result = generate(source, [ReportSpec("html", site)], root=tmp_path)
    assert result.written == ()
    assert result.problems == (
        f"html report not written, {site} is not empty and holds no earlier html report",
    )
    assert sorted(path.name for path in site.iterdir()) == ["index.html", "page.md", "style.css"]


@pytest.mark.parametrize("target", [".", "..", "sub/.."])
def test_html_keeps_the_rootdir_and_its_parents(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, target: str
) -> None:
    fake_genhtml(tmp_path, monkeypatch)
    root = tmp_path / "project"
    (root / "sub").mkdir(parents=True)
    (root / "index.html").write_text("mine")
    (root / "gcov.css").write_text("mine")
    source = root / "in.info"
    source.write_text(TRACEFILE)
    out = root / target
    result = generate(source, [ReportSpec("html", out)], root=root)
    assert result.written == ()
    assert result.problems == (f"html report not written, {out} holds the rootdir",)
    assert (root / "index.html").read_text() == "mine"
    assert source.exists()


def test_html_keeps_the_working_directory_the_home_and_the_coverage_data(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch)
    root = tmp_path / "root"
    root.mkdir()
    cases = {
        "the working directory": tmp_path / "cwd",
        "the home directory": tmp_path / "home",
        "the coverage data": tmp_path / "data",
    }
    for directory in cases.values():
        directory.mkdir()
        (directory / "index.html").write_text("mine")
        (directory / "gcov.css").write_text("mine")
    monkeypatch.chdir(cases["the working directory"])
    monkeypatch.setenv("HOME", str(cases["the home directory"]))
    source = cases["the coverage data"] / "in.info"
    source.write_text(TRACEFILE)
    for label, directory in cases.items():
        result = generate(source, [ReportSpec("html", directory)], root=root)
        assert result.written == ()
        assert result.problems == (f"html report not written, {directory} holds {label}",)
        assert (directory / "index.html").read_text() == "mine"


def test_html_keeps_a_symbolic_link_to_a_report(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    real = tmp_path / "real"
    real.mkdir()
    (real / "index.html").write_text("mine")
    (real / "gcov.css").write_text("mine")
    link = tmp_path / "link"
    link.symlink_to(real, target_is_directory=True)
    dangling = tmp_path / "dangling"
    dangling.symlink_to(tmp_path / "nowhere")
    for target in (link, dangling):
        result = generate(source, [ReportSpec("html", target)], root=tmp_path)
        assert result.written == ()
        assert result.problems == (f"html report not written, {target} is a symbolic link",)
    assert (real / "index.html").read_text() == "mine"
    assert not (tmp_path / "nowhere").exists()


def test_html_without_genhtml_is_written_by_the_builtin_renderer(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PATH", str(tmp_path))
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.problems == ()
    assert result.written == (out,)
    assert sorted(path.name for path in out.iterdir()) == [
        ".popcorn-html-report",
        "index.html",
        "src-0.html",
        "src-1.html",
        "style.css",
    ]


def test_html_builtin_keeps_the_target_rules_of_genhtml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PATH", str(tmp_path))
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    foreign = tmp_path / "foreign"
    foreign.mkdir()
    (foreign / "notes.txt").write_text("mine")
    (foreign / "index.html").write_text("mine")
    (foreign / "style.css").write_text("mine")
    result = generate(source, [ReportSpec("html", foreign)], root=tmp_path)
    assert result.written == ()
    assert sorted(path.name for path in foreign.iterdir()) == [
        "index.html",
        "notes.txt",
        "style.css",
    ]
    link = tmp_path / "link"
    link.symlink_to(foreign, target_is_directory=True)
    result = generate(source, [ReportSpec("html", link)], root=tmp_path)
    assert result.problems == (f"html report not written, {link} is a symbolic link",)
    assert (foreign / "notes.txt").read_text() == "mine"
    here = tmp_path / "here"
    here.mkdir()
    (here / "index.html").write_text("mine")
    (here / ".popcorn-html-report").write_text("")
    monkeypatch.chdir(here)
    result = generate(source, [ReportSpec("html", here)], root=tmp_path)
    assert result.problems == (f"html report not written, {here} holds the working directory",)
    assert (here / "index.html").read_text() == "mine"


def test_html_builtin_replaces_an_earlier_builtin_report_and_a_genhtml_one(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PATH", str(tmp_path))
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    assert generate(source, [ReportSpec("html", out)], root=tmp_path).problems == ()
    (out / "stale.txt").write_text("old")
    (out / "index.html").write_text("old")
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.problems == ()
    assert not (out / "stale.txt").exists()
    assert (out / "index.html").read_text() != "old"
    assert (out / ".popcorn-html-report").is_file()
    earlier = tmp_path / "genhtml"
    earlier.mkdir()
    (earlier / "index.html").write_text("old")
    (earlier / "gcov.css").write_text("old")
    assert generate(source, [ReportSpec("html", earlier)], root=tmp_path).problems == ()
    assert (earlier / "src-0.html").is_file()


def test_html_builtin_leaves_no_directory_when_it_cannot_write(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PATH", str(tmp_path))
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)

    def full(*_: object) -> str:
        raise OSError("disk full")

    monkeypatch.setattr("popcorn.coverage.report._html._source_page", full)
    out = tmp_path / "out"
    with pytest.raises(OSError):
        generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert not out.exists()


def test_html_uses_genhtml_when_installed(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert (out / "index.html").read_text() == "fake"
    assert not (out / "src-0.html").exists()


def test_html_target_survives_an_unknown_home(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PATH", str(tmp_path))

    def no_home() -> Path:
        raise RuntimeError("Could not determine home directory.")

    monkeypatch.setattr(Path, "home", no_home)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.problems == ()
    assert (out / "index.html").is_file()


@pytest.mark.parametrize("builtin", [True, False])
def test_html_keeps_a_report_directory_that_holds_a_git_repository(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, builtin: bool
) -> None:
    if builtin:
        monkeypatch.setenv("PATH", str(tmp_path))
    else:
        fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    site = tmp_path / "site"
    (site / ".git").mkdir(parents=True)
    (site / ".git" / "HEAD").write_text("ref")
    (site / "index.html").write_text("old")
    (site / "gcov.css").write_text("old")
    (site / ".popcorn-html-report").write_text("")
    result = generate(source, [ReportSpec("html", site)], root=tmp_path)
    assert result.written == ()
    assert result.problems == (f"html report not written, {site} holds a .git entry",)
    assert (site / ".git" / "HEAD").read_text() == "ref"
    assert (site / "index.html").read_text() == "old"


def test_html_keeps_an_existing_empty_target_after_a_failed_genhtml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch, code=1, index=False)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    out.mkdir()
    result = generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert result.problems == ("html report not written, genhtml exited 1: boom",)
    assert out.is_dir()
    assert list(out.iterdir()) == []


def test_html_cleans_up_when_genhtml_vanishes_after_the_check(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_genhtml(tmp_path, monkeypatch)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"

    def vanished(*_: object, **__: object) -> None:
        raise FileNotFoundError("genhtml is not installed")

    monkeypatch.setattr("popcorn.coverage.report._spec.run_genhtml", vanished)
    with pytest.raises(FileNotFoundError):
        generate(source, [ReportSpec("html", out)], root=tmp_path)
    assert not out.exists()


def _fake_spawning_genhtml(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    pids = tmp_path / "child.pid"
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    script = bin_dir / "genhtml"
    script.write_text(
        f"#!{sys.executable}\n"
        "import subprocess, sys, time\n"
        f"child = subprocess.Popen([{sys.executable!r}, '-c', 'import time; time.sleep(60)'])\n"
        f"open({str(pids)!r}, 'w').write(str(child.pid))\n"
        "time.sleep(60)\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("PATH", str(bin_dir))
    return pids


def _wait_for_pid(pids: Path) -> int:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if pids.exists() and pids.read_text():
            return int(pids.read_text())
        time.sleep(0.02)
    raise AssertionError("the fake genhtml wrote no pid")


def _is_gone(pid: int) -> bool:
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rpartition(")")[2].split()[0]
    except FileNotFoundError:
        return True
    return state in ("Z", "X")


def _assert_dies(pid: int) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not _is_gone(pid):
        time.sleep(0.05)
    assert _is_gone(pid)


def test_html_kills_the_children_of_a_timed_out_genhtml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    pids = _fake_spawning_genhtml(tmp_path, monkeypatch)
    monkeypatch.setattr("popcorn.coverage.report._genhtml.GENHTML_TIMEOUT", 3.0)
    monkeypatch.setattr("popcorn.coverage.report._spec.GENHTML_TIMEOUT", 3.0)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    result = generate(source, [ReportSpec("html", tmp_path / "out")], root=tmp_path)
    assert result.problems == ("html report not written, genhtml ran past 3 s",)
    _assert_dies(_wait_for_pid(pids))


def test_html_kills_genhtml_on_an_interrupt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    pids = _fake_spawning_genhtml(tmp_path, monkeypatch)
    real: Callable[..., object] = vars(subprocess.Popen)["communicate"]
    calls: list[int] = []

    def interrupted(self: subprocess.Popen[str], *args: object, **kwargs: object) -> object:
        calls.append(1)
        if len(calls) == 1:
            _wait_for_pid(pids)
            raise KeyboardInterrupt
        return real(self, *args, **kwargs)

    monkeypatch.setattr(subprocess.Popen, "communicate", interrupted)
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out"
    with pytest.raises(KeyboardInterrupt):
        generate(source, [ReportSpec("html", out)], root=tmp_path)
    _assert_dies(_wait_for_pid(pids))
    assert not out.exists()


def test_html_refuses_a_dangling_git_symlink(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PATH", str(tmp_path))
    site = tmp_path / "site"
    site.mkdir()
    (site / ".git").symlink_to(tmp_path / "nowhere")
    (site / "index.html").write_text("old")
    (site / "gcov.css").write_text("old")
    (site / ".popcorn-html-report").write_text("")
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    result = generate(source, [ReportSpec("html", site)], root=tmp_path)
    assert result.problems == (f"html report not written, {site} holds a .git entry",)
    assert (site / "index.html").read_text() == "old"
