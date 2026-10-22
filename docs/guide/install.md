# Install

popcorn installs from source. The native core compiles on your machine, so the first install
takes a few minutes.

## Prerequisites

On Linux x86_64:

- Python 3.14 or later, with its development headers, and uv.
- g++ 16.1 or later, as `g++`.
- cmake and ninja.
- git and network access. Unicorn and reflex are git submodules, and the build fetches the
  other third-party sources.
- Optional: arm-none-eabi-gcc, to rebuild the example and test firmware.

## Install the package

```sh
uv pip install "popcorn[pytest] @ git+https://github.com/Garcia6l20/popcorn"
```

The extras add optional dependencies:

- `pytest` installs pytest for the `popcorn.pytest` plugin.
- `plugin` installs pcons and ninja, to build native plugins in development mode.

Install both with `popcorn[pytest,plugin]`.

## Work on popcorn itself

Clone with the submodules. The build reads the Unicorn commit from git, so a copy of the tree
without its git metadata does not build.

```sh
git clone --recurse-submodules https://github.com/Garcia6l20/popcorn
cd popcorn
uv sync
uv run pcons -B build
```

`uv sync` installs popcorn in editable mode from `build/`. Run `uv run pcons -B build` again after
a change to native or Python sources.

## STM32 part descriptions

A machine needs the description of its part: memory map, core, peripherals and interrupts.
`popcorn.stm32.mcu(part)` returns it.

Shipped parts need no ST tool. popcorn ships the parts on the [Parts](../support/generated/parts.md)
page and decompresses each one on first use into `$XDG_CACHE_HOME/popcorn/mcu/shipped`,
`~/.cache` when the variable is unset.

For another part, popcorn builds the description from ST's files on first use and caches it
under `$XDG_CACHE_HOME/popcorn/mcu`. It reads:

- the CubeMX database, from `~/STM32CubeMX/db` or `POPCORN_CUBEMX_DB`.
- the SVD, from STM32CubeCLT or STM32CubeIDE under `~/st`, or from `POPCORN_STM32_SVD_DIRS`, a
  `:`-separated list.
- optionally, the CMSIS device header, from `~/STM32Cube/Repository/STM32Cube_FW_<family>_V*` or
  `POPCORN_CMSIS_DEVICE_DIRS`, for the CMSIS interrupt names.

`mcu()` takes the first of these sources:

1. `<RefName>.json`, or `<RefName>-<core>.json` for a multi-core part, in a directory of
   `POPCORN_STM32_DESCRIPTION_DIRS`, a `:`-separated list.
2. The shipped description, unless `svd=` is given or `POPCORN_STM32_BUILD=local` is set.
3. A description built from ST's files.

To see the ST files a build would read:

```sh
uv run python -m popcorn.stm32.description sources STM32U575QGIx
```

An unshipped part with no ST install raises an error that names the shipped parts and the
missing path.
