"""Reports over lcov tracefiles: the data model, parsing, filtering and counting.

```python
from pathlib import Path
from popcorn.coverage.report import parse_lcov

report = parse_lcov(Path("cov.info").read_text())
kept = report.filtered(["src/**"], ["**/tests/**"], Path.cwd())
print(kept.totals().lines.percent)
```
"""

from popcorn.coverage.report._model import (
    BranchRecord,
    Counts,
    FunctionRecord,
    LcovFile,
    LcovFormatError,
    LcovReport,
    Totals,
    display_name,
    matches,
    parse_lcov,
    resolve,
)

__all__ = [
    "BranchRecord",
    "Counts",
    "FunctionRecord",
    "LcovFile",
    "LcovFormatError",
    "LcovReport",
    "Totals",
    "display_name",
    "matches",
    "parse_lcov",
    "resolve",
]
