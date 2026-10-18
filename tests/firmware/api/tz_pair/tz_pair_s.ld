MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 8K
  NSC   (rx)  : ORIGIN = 0x08002000, LENGTH = 512
  SRAM  (rwx) : ORIGIN = 0x30030000, LENGTH = 16K
}

ENTRY(Reset_Handler)
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .gnu.sgstubs : ALIGN(32) { KEEP(*(.gnu.sgstubs*)) } > NSC
  .data : { *(.data*) } > SRAM
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM
}

__sgstubs_start = 0x08002000;
__sgstubs_end = 0x08002200;

ASSERT(ADDR(.gnu.sgstubs) == __sgstubs_start, "the veneers start the Non-secure callable range")
ASSERT(SIZEOF(.data) == 0, "the secure part keeps no initialized data")
