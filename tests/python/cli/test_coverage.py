import dataclasses
import errno
import json
from pathlib import Path

import pytest
from click.testing import CliRunner
from popcorn.cli import cli, main
from popcorn.coverage import (
    BlockHit,
    CodeRange,
    Coverage,
    CoverageGroup,
    EdgeHit,
    ImageRef,
    UnmodelledAccess,
    UnmodelledReason,
    merge,
)
from popcorn.coverage.report import parse_lcov

LINES_ELF = Path(__file__).parents[2] / "firmware" / "obs" / "prebuilt" / "lines.elf"


def make(
    hits: int, *, extra: tuple[BlockHit, ...] = (), unmodelled: tuple[UnmodelledAccess, ...] = ()
) -> Coverage:
    return Coverage.from_parts(
        [
            CoverageGroup(
                images=(ImageRef.from_elf(LINES_ELF),),
                ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                blocks=(BlockHit(0x0800_0008, 4, hits), *extra),
                edges=(),
                unmodelled=unmodelled,
            )
        ]
    )


def run(*args: str | Path):
    return CliRunner().invoke(cli, ["coverage", *map(str, args)])


def test_merge_of_files_equals_the_library_merge(tmp_path: Path) -> None:
    files: list[Path] = []
    for index, hits in enumerate((1, 2, 4)):
        path = tmp_path / f"{index}.pcov"
        make(hits, extra=(BlockHit(0x0800_0014 + 4 * index, 4, 1),)).save(path)
        files.append(path)
    out = tmp_path / "all.pcov"
    result = run("merge", *files, "-o", out)
    assert result.exit_code == 0
    assert Coverage.load(out) == merge(files)
    assert Coverage.load(out) != Coverage.load(files[0])
    assert sum(b.hits for b in Coverage.load(out).groups[0].blocks if b.start == 0x0800_0008) == 7


def test_merge_without_input_exits_2(tmp_path: Path) -> None:
    result = run("merge", "-o", tmp_path / "x.pcov")
    assert result.exit_code == 2
    assert not (tmp_path / "x.pcov").exists()


def test_merge_without_output_exits_2(tmp_path: Path) -> None:
    make(1).save(tmp_path / "a.pcov")
    assert run("merge", tmp_path / "a.pcov").exit_code == 2


def test_merge_missing_input_exits_2(tmp_path: Path) -> None:
    result = run("merge", tmp_path / "none.pcov", "-o", tmp_path / "x.pcov")
    assert result.exit_code == 2


def test_merge_names_a_bad_file_and_writes_nothing(tmp_path: Path) -> None:
    good = tmp_path / "good.pcov"
    make(1).save(good)
    bad = tmp_path / "bad.pcov"
    bad.write_bytes(b"not a pcov")
    out = tmp_path / "x.pcov"
    assert main(["coverage", "merge", str(good), str(bad), "-o", str(out)]) == 1
    assert not out.exists()
    result = run("merge", good, bad, "-o", out)
    assert result.exit_code == 1
    assert str(bad) in result.stderr


def test_merge_output_in_a_missing_directory_exits_1(tmp_path: Path) -> None:
    make(1).save(tmp_path / "a.pcov")
    result = run("merge", tmp_path / "a.pcov", "-o", tmp_path / "no" / "x.pcov")
    assert result.exit_code == 1
    assert "Error:" in result.stderr


def test_merge_output_that_is_a_directory_exits_2(tmp_path: Path) -> None:
    make(1).save(tmp_path / "a.pcov")
    assert run("merge", tmp_path / "a.pcov", "-o", tmp_path).exit_code == 2


def test_export_lcov_is_readable_and_branches_follow_the_flag(tmp_path: Path) -> None:
    pcov = tmp_path / "a.pcov"
    Coverage.from_parts(
        [
            CoverageGroup(
                images=(ImageRef.from_elf(LINES_ELF),),
                ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                blocks=(BlockHit(0x0800_0008, 4, 2), BlockHit(0x0800_0014, 4, 1)),
                edges=(EdgeHit(0x0800_0008, 0x0800_0014, 1),),
            )
        ]
    ).save(pcov)
    with_branches = tmp_path / "a.info"
    result = run("export", pcov, "--lcov", with_branches, "--test-name", "tn")
    assert result.exit_code == 0
    text = with_branches.read_text()
    assert text.splitlines()[0] == "TN:tn"
    assert "BRDA:9,0,0,1" in text.splitlines()
    report = parse_lcov(text)
    assert report.files
    without = tmp_path / "b.info"
    assert run("export", pcov, "--lcov", without, "--no-branches").exit_code == 0
    assert "BRDA" not in without.read_text()
    assert "DA:9,2" in without.read_text().splitlines()


def test_export_drcov_count_goes_to_stderr(tmp_path: Path) -> None:
    pcov = tmp_path / "a.pcov"
    make(1, extra=(BlockHit(0x2000_0000, 2, 1),)).save(pcov)
    out = tmp_path / "a.drcov"
    expected = tmp_path / "lib.drcov"
    dropped = Coverage.load(pcov).export_drcov(expected)
    assert dropped == 1
    result = run("export", pcov, "--drcov", out)
    assert result.exit_code == 0
    assert out.read_bytes() == expected.read_bytes()
    assert result.stderr == f"{out}: 1 block outside every image was left out\n"
    assert result.stdout == ""


def test_export_unmodelled_writes_the_library_sidecar(tmp_path: Path) -> None:
    access = UnmodelledAccess(0x0800_0008, "UART1", "CR1", 0, False, UnmodelledReason.NO_MODEL, 3)
    pcov = tmp_path / "a.pcov"
    coverage = make(1, unmodelled=(access,))
    coverage.save(pcov)
    expected = tmp_path / "lib.json"
    coverage.export_unmodelled(expected)
    out = tmp_path / "u.json"
    assert run("export", pcov, "--unmodelled", out).exit_code == 0
    assert out.read_text() == expected.read_text()
    assert json.loads(out.read_text())["files"]


def test_export_writes_every_requested_output(tmp_path: Path) -> None:
    pcov = tmp_path / "a.pcov"
    make(1).save(pcov)
    outs = [tmp_path / "a.info", tmp_path / "a.drcov", tmp_path / "a.json"]
    result = run("export", pcov, "--lcov", outs[0], "--drcov", outs[1], "--unmodelled", outs[2])
    assert result.exit_code == 0
    assert all(path.exists() for path in outs)


def test_export_without_an_output_option_exits_2(tmp_path: Path) -> None:
    pcov = tmp_path / "a.pcov"
    make(1).save(pcov)
    result = run("export", pcov)
    assert result.exit_code == 2
    assert "at least one" in result.stderr
    assert "--no-branches" not in result.stdout
    assert run("export", pcov, "--no-branches", "--test-name", "x").exit_code == 2


def test_export_of_a_non_pcov_file_exits_1_naming_it(tmp_path: Path) -> None:
    bad = tmp_path / "bad.pcov"
    bad.write_bytes(b"junk")
    out = tmp_path / "a.info"
    result = run("export", bad, "--lcov", out)
    assert result.exit_code == 1
    assert str(bad) in result.stderr
    assert not out.exists()


def test_export_missing_input_exits_2(tmp_path: Path) -> None:
    assert run("export", tmp_path / "none.pcov", "--lcov", tmp_path / "a.info").exit_code == 2


@pytest.mark.parametrize("option", ["--lcov", "--drcov", "--unmodelled"])
def test_export_output_that_is_a_directory_exits_2(tmp_path: Path, option: str) -> None:
    pcov = tmp_path / "a.pcov"
    make(1).save(pcov)
    assert run("export", pcov, option, tmp_path).exit_code == 2


@pytest.mark.parametrize("option", ["--lcov", "--drcov", "--unmodelled"])
def test_export_output_in_a_missing_directory_exits_1(tmp_path: Path, option: str) -> None:
    pcov = tmp_path / "a.pcov"
    make(1).save(pcov)
    result = run("export", pcov, option, tmp_path / "no" / "out")
    assert result.exit_code == 1
    assert "Error:" in result.stderr
    assert "Traceback" not in result.stderr


def test_export_lcov_of_a_changed_elf_exits_1(tmp_path: Path) -> None:
    image = dataclasses.replace(ImageRef.from_elf(LINES_ELF), digest="0")
    pcov = tmp_path / "a.pcov"
    Coverage.from_parts([CoverageGroup(images=(image,), ranges=(), blocks=(), edges=())]).save(pcov)
    result = run("export", pcov, "--lcov", tmp_path / "a.info")
    assert result.exit_code == 1
    assert "changed since" in result.stderr
    assert "Error:" in result.stderr


def test_unwritable_output_through_main_is_exit_1(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    pcov = tmp_path / "a.pcov"
    make(1).save(pcov)

    def refuse(self: Coverage, path: object, **kwargs: object) -> None:
        raise OSError(errno.ENOSPC, "no space left")

    monkeypatch.setattr(Coverage, "export_lcov", refuse)
    assert main(["coverage", "export", str(pcov), "--lcov", str(tmp_path / "a.info")]) == 1


def test_stdout_is_unused(tmp_path: Path) -> None:
    pcov = tmp_path / "a.pcov"
    make(1).save(pcov)
    assert run("export", pcov, "--lcov", tmp_path / "a.info").stdout == ""


def test_export_drcov_says_nothing_when_no_block_was_left_out(tmp_path: Path) -> None:
    pcov = tmp_path / "a.pcov"
    make(1).save(pcov)
    result = run("export", pcov, "--drcov", tmp_path / "a.drcov")
    assert result.exit_code == 0
    assert result.stderr == ""


def test_export_drcov_count_is_plural_for_two_blocks(tmp_path: Path) -> None:
    pcov = tmp_path / "a.pcov"
    make(1, extra=(BlockHit(0x2000_0000, 2, 1), BlockHit(0x2000_0010, 2, 1))).save(pcov)
    out = tmp_path / "a.drcov"
    result = run("export", pcov, "--drcov", out)
    assert result.stderr == f"{out}: 2 blocks outside every image were left out\n"


def test_export_lcov_of_a_deleted_elf_exits_1_naming_it(tmp_path: Path) -> None:
    elf = tmp_path / "fw.elf"
    elf.write_bytes(LINES_ELF.read_bytes())
    pcov = tmp_path / "a.pcov"
    Coverage.from_parts(
        [
            CoverageGroup(
                images=(ImageRef.from_elf(elf),),
                ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                blocks=(BlockHit(0x0800_0008, 4, 1),),
                edges=(),
            )
        ]
    ).save(pcov)
    elf.unlink()
    out = tmp_path / "a.info"
    result = run("export", pcov, "--lcov", out)
    assert result.exit_code == 1
    assert "Error:" in result.stderr
    assert str(elf) in result.stderr
    assert not out.exists()
