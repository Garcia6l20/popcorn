# Memory map

- On the STM32U5 and STM32H5 nothing is mapped at address 0. Under the default `faults="stop"`, a
  load, store or fetch there stops the run with a bus fault.
- The ICACHE remap of external memories onto the code region is not modelled. CRR0 to CRR3 store
  what the firmware writes and map nothing.
- On the STM32L1 and STM32L4 the main flash is aliased at address 0 over the size of the Flash
  region, RM0038 2.7 and RM0351 2.6.1, and VTOR resets to 0. The second bank of an STM32L1 Cat.6
  part reads at 0x00040000 to 0x0006FFFF, inferred. A CPU, DMA or debugger load and a fetch
  through the alias reach the flash under the PCROP, bank lock and read protection rules of the
  flash address, inferred: at RDP level 1 on the STM32L4 a debugger access through the alias is
  refused like one at 0x08000000. A CPU or DMA store through the alias is refused, a bus fault for
  the CPU and a transfer error for the DMA, inferred.
- SYSCFG_MEMRMP MEM_MODE, and FB_MODE on the STM32L4, are storage. Address 0 always maps the main
  flash.
- With `swap_bank` address 0 also maps the main flash. On silicon the system memory stays aliased
  at 0 with BFB2 set, RM0351 9.2.1, and with nBFB2 0, RM0038 Table 6.
