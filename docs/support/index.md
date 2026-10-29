# Support

These pages say which parts, cores and peripherals popcorn emulates. The tables are generated
from the shipped descriptions and the model registry at each docs build, so they follow the code.

- [Parts](generated/parts.md): the parts that ship with a description and the state of each, with
  a page per part of its peripheral instances, their models and its bonded pins.
- [Cores](generated/cores.md): the arch modules and the core of each part.
- [Limits](limits.md): what each model leaves out, and where it differs from the reference manual.

## What the states mean

A part is supported when firmware examples run on its class: its die, flash size, package and
SMPS supply. The dies are U575/U585 and H56x/H573. The state column of the
[Parts](generated/parts.md) page names the evidence:

- **tested**: the part runs the whole firmware suites of the examples, the STM32U575QGIx and the
  STM32H573AIIx.
- **examples**: the part runs the listed examples itself, as the representative of its class.
- **class**: the part shares its class with the named part, which runs examples.
- **attaches**: the part ships and attaches, and its host checks are its evidence.

A note on a row, FLASH geometry cases, marks the two parts the FLASH model's geometry cases run
on, a 2 MB U5 part and a 1 MB H5 part. Every shipped part attaches at TrustZone 0 and 1, and its
flash end, SRAM ends and bonded pins are checked on the host, whatever its state. Each part page
lists the pins its package bonds on every GPIO port.

## What "modelled" means

An instance with a model has its registers served by code that reproduces the peripheral's
behaviour: flags, interrupts, DMA requests, timing and its link to the outside, such as a UART
line or an I2C target. An instance without a model keeps plain register storage. A firmware
access to it logs a warning, and raises `ModelError` under `strict`.

A model does not make every field behaviour-exact. Each model covers the features the firmware
of the examples and the tests use. [Limits](limits.md) lists the rest.

The model column of a part page reads:

- **built-in**: a native model ships with popcorn and serves the instance, unless a Python
  model is registered for its IP.
- **built-in, TrustZone only**: the model serves the instance only when the machine boots with
  TrustZone enabled.
- **none**: no model ships. A test can still serve the instance with a Python model or a native
  plugin model.

The same data is available at run time:

```python
--8<-- "test_support_pages.py:query"
```
