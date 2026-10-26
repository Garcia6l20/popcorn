"""Reports over lcov tracefiles: the data model, parsing, filtering and counting.

```python
from pathlib import Path
from popcorn.coverage.report import parse_lcov

report = parse_lcov(Path("cov.info").read_text())
kept = report.filtered(["src/**"], ["**/tests/**"], Path.cwd())
print(kept.totals().lines.percent)
```
"""

from popcorn.coverage.report._cobertura import render_cobertura
from popcorn.coverage.report._html import render_html
from popcorn.coverage.report._model import (
    BranchRecord,
    Counts,
    FunctionRecord,
    LcovFile,
    LcovFormatError,
    LcovReport,
    Totals,
    below,
    display_name,
    matches,
    parse_lcov,
    resolve,
    truncated_percent,
)
from popcorn.coverage.report._spec import ReportResult, ReportSpec, generate, parse_spec
from popcorn.coverage.report._term import render_term

__all__ = [
    "BranchRecord",
    "Counts",
    "FunctionRecord",
    "LcovFile",
    "LcovFormatError",
    "LcovReport",
    "ReportResult",
    "ReportSpec",
    "Totals",
    "below",
    "display_name",
    "generate",
    "matches",
    "parse_lcov",
    "parse_spec",
    "render_cobertura",
    "render_html",
    "render_term",
    "resolve",
    "truncated_percent",
]
