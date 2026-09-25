from pathlib import Path

import pytest

_CANDIDATES = [
    Path("~/st/stm32cubeclt_1.20.0/STMicroelectronics_CMSIS_SVD/STM32U575.svd").expanduser(),
    Path("~/st/stm32cubeide_2.0.0")
    .expanduser()
    .glob(
        "plugins/com.st.stm32cube.ide.mcu.productdb.debug_*/resources/cmsis/"
        "STMicroelectronics_CMSIS_SVD/STM32U575.svd"
    ),
]


@pytest.fixture(scope="session")
def st_svd_u575() -> Path:
    """Path of the local ST STM32U575.svd install, skips naming every path searched."""
    searched: list[str] = []
    for candidate in _CANDIDATES:
        if isinstance(candidate, Path):
            searched.append(str(candidate))
            if candidate.is_file():
                return candidate
        else:
            for match in candidate:
                searched.append(str(match))
                if match.is_file():
                    return match
    pytest.skip(f"STM32U575.svd not found, searched: {', '.join(searched)}")


_CUBEMX_DB = Path("~/STM32CubeMX/db").expanduser()


@pytest.fixture(scope="session")
def st_cubemx_db() -> Path:
    """Path of the local CubeMX db directory, skips naming the path searched."""
    if _CUBEMX_DB.is_dir():
        return _CUBEMX_DB
    pytest.skip(f"CubeMX db not found, searched: {_CUBEMX_DB}")
