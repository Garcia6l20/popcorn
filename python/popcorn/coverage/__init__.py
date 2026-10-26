"""Coverage from popcorn machines: .pcov files, merging across tests and workers, lcov export."""

from popcorn.coverage._coverage import (
    BlockHit,
    CodeRange,
    Coverage,
    CoverageFileError,
    CoverageGroup,
    EdgeHit,
    ImageKind,
    ImageRef,
    merge,
    merge_session,
    worker_path,
)
from popcorn.coverage.report import LcovFormatError, LcovReport, Totals, parse_lcov

__all__ = [
    "BlockHit",
    "CodeRange",
    "Coverage",
    "CoverageFileError",
    "CoverageGroup",
    "EdgeHit",
    "ImageKind",
    "ImageRef",
    "LcovFormatError",
    "LcovReport",
    "Totals",
    "merge",
    "merge_session",
    "parse_lcov",
    "worker_path",
]
