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

__all__ = [
    "BlockHit",
    "CodeRange",
    "Coverage",
    "CoverageFileError",
    "CoverageGroup",
    "EdgeHit",
    "ImageKind",
    "ImageRef",
    "merge",
    "merge_session",
    "worker_path",
]
