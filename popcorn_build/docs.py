import sys

from popcorn_build.config import project_dir


def build_command() -> list[str]:
    """The strict docs build: any warning, a broken link or a missing snippet fails it.

    mkdocstrings reads the package from build/popcorn, so the build stage runs first."""
    return [
        sys.executable,
        "-m",
        "zensical",
        "build",
        "--strict",
        "--config-file",
        str(project_dir / "mkdocs.yml"),
    ]
