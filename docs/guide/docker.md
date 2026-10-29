# Docker

The image `ghcr.io/garcia6l20/popcorn` runs popcorn tests and scripts with no compiler on the
host. It holds Python 3.14, popcorn with its `pytest` extra, and uv, on Linux x86_64. The tag
`main` follows the main branch.

```sh
docker pull ghcr.io/garcia6l20/popcorn:main
```

## Run a pytest suite

Mount the suite at `/work`, the working directory, and run as your own uid:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" ghcr.io/garcia6l20/popcorn:main pytest
```

With no command the image runs `pytest`. Options need the command, as in
`docker run ... ghcr.io/garcia6l20/popcorn:main pytest -q`. The popcorn plugin keeps its boot
snapshots in `/work/.pytest_cache`, so `/work` must be writable by the uid given with `--user`.
Without `--user`, the container runs as uid 1000. To run a suite from a read-only mount, turn the
cache off with `pytest -p no:cacheprovider`.

The examples of a checkout run the same way:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/examples/stm32u575/gpio:/work" ghcr.io/garcia6l20/popcorn:main pytest -q
```

## Run a script

A script runs with `python`. popcorn takes absolute firmware paths, so build them from the script's
location, such as `Path(__file__).parent / "firmware.elf"`, and they resolve inside the container.
[First machine](first_machine.md) shows a complete script. Run one with:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" ghcr.io/garcia6l20/popcorn:main python run_firmware.py
```

## Caches

The description cache lives under `/cache`, a volume. A named volume keeps it across runs:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" -v popcorn-cache:/cache ghcr.io/garcia6l20/popcorn:main pytest
```

Use one cache volume per uid. The first uid that writes the volume owns `/cache/popcorn`, and only
that uid adds descriptions to it.

## Other parts

The image holds the shipped parts listed on the [Parts](../support/generated/parts.md) page, and
no ST files. For another part, mount your ST installation and point popcorn at it, as
[Install](install.md#stm32-part-descriptions) describes:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" -v popcorn-cache:/cache \
    -v "$HOME/STM32CubeMX/db:/st/db:ro" -e POPCORN_CUBEMX_DB=/st/db \
    -v "$HOME/st/stm32cubeclt_1.20.0/STMicroelectronics_CMSIS_SVD:/st/svd:ro" -e POPCORN_STM32_SVD_DIRS=/st/svd \
    -v "$HOME/STM32Cube/Repository/STM32Cube_FW_U5_V1.8.0/Drivers/CMSIS/Device/ST/STM32U5xx/Include:/st/cmsis:ro" \
    -e POPCORN_CMSIS_DEVICE_DIRS=/st/cmsis \
    ghcr.io/garcia6l20/popcorn:main pytest
```

`POPCORN_CMSIS_DEVICE_DIRS` names the directories that hold the `stm32<family>xx.h` headers and is
optional. A directory of prebuilt `<RefName>.json` descriptions needs no ST installation:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" \
    -v "$PWD/descriptions:/descriptions:ro" -e POPCORN_STM32_DESCRIPTION_DIRS=/descriptions \
    ghcr.io/garcia6l20/popcorn:main pytest
```

## Add packages

The image has no compiler, so [native plugins](native_plugins.md) build on the host. Python
packages install with uv in a derived image, into the image's environment:

```dockerfile
FROM ghcr.io/garcia6l20/popcorn:main
USER root
RUN uv pip install --no-cache pytest-xdist
USER popcorn
```

## Build the image locally

Build from a fresh clone with its submodules. The build context takes whole source directories,
so the untracked files of a working checkout would reach the image, and a git worktree lacks the
submodule metadata the build reads.

```sh
git clone --recurse-submodules https://github.com/Garcia6l20/popcorn popcorn-image
cd popcorn-image
docker buildx build -f docker/Dockerfile --build-arg JOBS="$(nproc)" -t popcorn:local --load .
```

## Licences and source

The licences of popcorn, of the components it builds in and of the GCC runtime libraries are under
`/usr/share/doc/popcorn`, with `IMAGE_NOTICE` describing each part. The image label
`org.opencontainers.image.revision` names the popcorn commit it was built from:

```sh
docker image inspect --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' ghcr.io/garcia6l20/popcorn:main
```

The Unicorn source is the commit of https://github.com/Garcia6l20/unicorn, branch `popcorn`, that the
`third_party/unicorn` submodule pins at that revision.
