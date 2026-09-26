from collections.abc import Mapping
from typing import Any


def is_firmware_only(entry: Mapping[str, Any]) -> bool:
    """Whether a deps.toml package is used by test firmware only, never by popcorn itself."""
    return entry.get("firmware", False) is True


def packages_to_fetch(manifest: Mapping[str, Mapping[str, Any]], with_firmware: bool) -> list[str]:
    """deps.toml packages a configure needs, the firmware-only ones only @p with_firmware."""
    return [
        name for name, entry in manifest.items() if with_firmware or not is_firmware_only(entry)
    ]
