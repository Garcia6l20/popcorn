"""Reports over lcov tracefiles: the data model, parsing, filtering and counting.

```python
from pathlib import Path
from popcorn.coverage.report import parse_lcov

report = parse_lcov(Path("cov.info").read_text())
kept = report.filtered(["src/**"], ["**/tests/**"], Path.cwd())
print(kept.totals().lines.percent)
```

`generate` writes the reports of one lcov file, as the pytest plugin does at the end of a session:

```python
from pathlib import Path
from popcorn.coverage.report import generate, parse_spec

result = generate(
    Path("cov.info"),
    [parse_spec("term"), parse_spec("html:htmlcov"), parse_spec("xml:coverage.xml")],
    include=["**/firmware/**"],
    omit=["**/vendor/**"],
    root=Path.cwd(),
)
print(result.term)
```

An include or omit glob matches a source path as the lcov file writes it and relative to `root`.
`**/tests/**` matches `tests/a.c` and `/home/u/x/tests/a.c`, where `*/tests/*` matches neither.
"""

from popcorn.coverage.report._cobertura import render_cobertura
from popcorn.coverage.report._gcovr import GCOVR_FORMAT_VERSION, render_gcovr_json
from popcorn.coverage.report._html import render_html
from popcorn.coverage.report._model import (
    UNMODELLED_MODES,
    BranchRecord,
    Counts,
    FunctionRecord,
    LcovFile,
    LcovFormatError,
    LcovReport,
    Totals,
    UnmodelledUse,
    below,
    display_name,
    dump_unmodelled,
    matches,
    parse_lcov,
    parse_unmodelled,
    resolve,
    truncated_percent,
)
from popcorn.coverage.report._spec import ReportResult, ReportSpec, generate, parse_spec
from popcorn.coverage.report._term import render_term

__all__ = [
    "GCOVR_FORMAT_VERSION",
    "UNMODELLED_MODES",
    "BranchRecord",
    "Counts",
    "FunctionRecord",
    "LcovFile",
    "LcovFormatError",
    "LcovReport",
    "ReportResult",
    "ReportSpec",
    "Totals",
    "UnmodelledUse",
    "below",
    "display_name",
    "dump_unmodelled",
    "generate",
    "matches",
    "parse_lcov",
    "parse_spec",
    "parse_unmodelled",
    "render_cobertura",
    "render_gcovr_json",
    "render_html",
    "render_term",
    "resolve",
    "truncated_percent",
]
