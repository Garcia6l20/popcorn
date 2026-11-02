MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
  SRAM2 (rwx) : ORIGIN = 0x10000000, LENGTH = 32K
}

ENTRY(Reset_Handler)
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .data : { *(.data*) *(.RamFunc) *(.RamFunc*) . = ALIGN(4); } > SRAM AT > FLASH
  .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(4); } > SRAM
  .sram2 (NOLOAD) : { *(.sram2) *(.sram2*) . = ALIGN(4); } > SRAM2
}
