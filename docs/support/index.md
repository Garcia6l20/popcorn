# Support

These pages say which parts, cores and peripherals popcorn emulates. The tables are generated
from the shipped descriptions and the model registry at each docs build, so they follow the code.

- [Parts](generated/parts.md): the parts that ship with a description, each with a page of its
  peripheral instances and their models.
- [Cores](generated/cores.md): the arch modules and the core of each part.
- [Limits](limits.md): what each model leaves out, and where it differs from the reference manual.

## What "modelled" means

An instance with a model has its registers served by code that reproduces the peripheral's
behaviour: flags, interrupts, DMA requests, timing and its link to the outside, such as a UART
line or an I2C target. An instance without a model keeps plain register storage. A firmware
access to it logs a warning, and raises `ModelError` under `strict`.

A model does not make every field behaviour-exact. Each model covers the features the firmware
of the examples and the tests use. [Limits](limits.md) lists the rest.

The model column of a part page reads:

- **built-in**: a native model ships with popcorn and serves the instance in every machine.
- **built-in, TrustZone only**: the model serves the instance only when the machine boots with
  TrustZone enabled.
- **none**: no model ships. A test can still serve the instance with a Python model or a native
  plugin model.

The same data is available at run time:

```python
--8<-- "test_support_pages.py:query"
```
