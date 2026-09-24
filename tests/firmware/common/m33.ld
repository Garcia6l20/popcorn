MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 768K
}

ENTRY(Reset_Handler)
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .data : { *(.data*) . = ALIGN(4); } > SRAM AT > FLASH
  .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(4); } > SRAM
}
