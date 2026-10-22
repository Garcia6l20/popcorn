from popcorn import _core


def arch_modules() -> list[str]:
    """Names of the arch modules registered now, such as `armv8m`, the latest registered last.

    A machine takes the latest one that implements its part's core.
    """
    return _core.arch_modules()
