# Limits

A built-in model serves the registers and behaviour the firmware of the examples and the tests
use. It does not serve every field of every register. Each page below lists what its model
leaves out and the deviations from the reference manual, RM0456 for the STM32U5, that its source
documents. The class doc in the linked source has the detail.

Most models meet an unmodelled setting with one `model_warning` log record per instance, then
keep running as the page says. Under `strict` each such warning raises `ModelError` instead.
The MDF and ADF model raises `UnmodelledConfig`. Read the log records of a run to see which
settings a firmware reached.

On the STM32H5 a Machine runs the STM32H562, H563 and H573. The STM32H503, H523 and H533
descriptions build from their SVD, and attaching a Machine to them raises `ConfigError`. The
STM32H543 and H553 descriptions need ST's DIE47C rzone file to build.

## General pages

- [Peripheral instances](unmodelled.md): the peripherals without a model and the instances a
  package drops.
- [Memory map](memory.md): address 0, the flash alias and the ICACHE remap.
- [Core](../generated/limits/core.md): the faults that stop the run and the reset requests.
- [STM32L4](stm32l4.md): the core errata, the STM32L485 and the instances without a model.
- [STM32L1](stm32l1.md): the patches of the shipped descriptions.

The family pages [STM32L1](../stm32l1.md) and [STM32L4](../stm32l4.md) describe the families.
The pages of this section carry what the models of those families leave out.

## Peripherals

Each page lists the deviations of one peripheral model by family.

--8<-- "limits_table.inc"
