import re
import shutil
import subprocess
import sys

from popcorn_build.config import project_dir

PACKAGE = project_dir / "build" / "popcorn" / "__init__.py"
GENERATED = project_dir / "docs" / "support" / "generated"

_ANSI = re.compile(r"\x1b\[[0-9;]*m")


def build_command() -> list[str]:
    """The strict Zensical build. `--strict` fails it on a broken link or anchor, and a missing
    snippet or reference object fails it on its own. griffe's docstring warnings do not fail
    it, `flagged()` catches those."""
    return [
        sys.executable,
        "-m",
        "zensical",
        "build",
        "--strict",
        "--config-file",
        str(project_dir / "mkdocs.yml"),
    ]


def flagged(line: str) -> bool:
    """Whether a line of the build output is a warning the docs stage fails on."""
    plain = _ANSI.sub("", line)
    return plain.startswith("griffe:") or "WARNING" in plain.upper()


def generate() -> int:
    """Regenerate the support pages into docs/support/generated, in an interpreter of its own so
    only the built-in models count. The return code of that interpreter."""
    shutil.rmtree(GENERATED, ignore_errors=True)
    return subprocess.run(
        [sys.executable, "-m", "popcorn_build.support_pages", str(GENERATED)],
        cwd=project_dir,
        check=False,
    ).returncode


def run() -> int:
    """Generate the support pages, run the build, stream its output, and return 1 on a warning
    line or a failed step.

    mkdocstrings and the support pages read the package from build/popcorn, so the native build
    comes first."""
    if not PACKAGE.is_file():
        print(
            f"popcorn docs: {PACKAGE.parent} is missing, run `uv run pcons -B build` first",
            file=sys.stderr,
        )
        return 1
    generated = generate()
    if generated != 0:
        return generated
    warnings: list[str] = []
    with subprocess.Popen(
        build_command(),
        cwd=project_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    ) as build:
        assert build.stdout is not None
        for line in build.stdout:
            sys.stdout.write(line)
            if flagged(line):
                warnings.append(line.rstrip())
    if build.returncode != 0:
        return build.returncode
    if warnings:
        print(f"popcorn docs: {len(warnings)} warning line(s) fail the build", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(run())
