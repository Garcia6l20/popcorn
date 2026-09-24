# popcorn

popcorn runs STM32 Cortex-M firmware on the Unicorn CPU emulator so it can be unit tested from
Python. A test loads the firmware ELF, calls single functions or boots the whole image with
emulated peripherals, drives inputs and asserts on outputs, memory, registers and peripheral
state. A pytest plugin boots once and restores a snapshot per test.

## Install

popcorn is installed from source, the native core is compiled on your machine.

Prerequisites on Linux x86_64:

- Python >= 3.14 with its development headers, and uv
- g++ >= 16.1 as `g++`
- cmake and ninja
- git and network access, third-party sources are fetched at build time
- optional: arm-none-eabi-gcc, to rebuild the test firmware

```sh
uv pip install git+<repository-url>
```

## License

MIT, see `LICENSE`. Third-party components and their licences are listed in `NOTICE`.
