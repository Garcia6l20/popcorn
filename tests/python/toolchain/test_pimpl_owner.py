import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PIMPL_IN_UNIQUE_PTR = re.compile(r"std::unique_ptr<\s*impl\s*>")


def test_no_public_header_holds_a_pimpl_in_a_unique_ptr() -> None:
    """Matches the type name `impl` only, a pimpl struct named otherwise is not seen."""
    holders = [
        str(header.relative_to(ROOT))
        for header in sorted((ROOT / "native" / "include").rglob("*.hpp"))
        if PIMPL_IN_UNIQUE_PTR.search(header.read_text())
    ]
    assert holders == []
