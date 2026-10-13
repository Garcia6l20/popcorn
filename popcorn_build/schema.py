import json
import subprocess
import sys

import click
from pcons import cli_command

from popcorn_build.config import build_dir, project_dir

SCHEMA_FILE = project_dir / "data" / "schema" / "part_description.schema.json"
GENERATOR = "popcorn-describe-schema"


def regenerate() -> None:
    """Build the schema generator and rewrite the committed part description JSON Schema."""
    subprocess.run(
        [sys.executable, "-m", "pcons", "-B", str(build_dir), GENERATOR],
        cwd=project_dir,
        check=True,
    )
    generated = subprocess.run(
        [str(build_dir / GENERATOR)], capture_output=True, text=True, check=True
    ).stdout
    SCHEMA_FILE.write_text(json.dumps(json.loads(generated), indent=2) + "\n")
    click.echo(f"schema: wrote {SCHEMA_FILE.relative_to(project_dir)}")


def register() -> None:
    @cli_command()
    def schema() -> None:
        """Regenerate data/schema/part_description.schema.json from the C++ aggregates."""
        regenerate()
