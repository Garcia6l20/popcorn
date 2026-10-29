MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

ENTRY(Reset_Handler)
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .data : { *(.data*) *(.RamFunc) *(.RamFunc*) . = ALIGN(4); } > SRAM AT > FLASH
  .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(4); } > SRAM
}
