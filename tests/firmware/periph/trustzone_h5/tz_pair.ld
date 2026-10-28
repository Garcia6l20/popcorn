MEMORY
{
  SECURE (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
  FLASH  (rx)  : ORIGIN = 0x08010000, LENGTH = 960K
  SRAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 192K
}

ENTRY(Reset_Handler)
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
  .secure_image : { KEEP(*(.secure_image)) } > SECURE
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .data : { *(.data*) } > SRAM
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM
}

ASSERT(ADDR(.secure_image) == 0x08000000, "the secure binary starts the flash")
ASSERT(ADDR(.isr_vector) == 0x08010000, "the Non-secure vectors start sector 8")
ASSERT(SIZEOF(.data) == 0, "the Non-secure part keeps no initialized data")
