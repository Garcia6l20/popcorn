# STM32L1

## STM32L1 description

Source: [patches](https://github.com/Garcia6l20/popcorn/blob/main/python/popcorn/stm32/description/patches)

The shipped STM32L1 descriptions start from the SVDs and the CubeMX part files. Patches correct
them from RM0038 and the CMSIS device headers. These corrections are stated by RM0038 or the
headers:

- The flash size of a code comes from its CubeMX part file, and RM0038 3.2 gives the category
  maximum only.
- `RCC_APB2LPENR` resets to 0x521D, RM0038 6.3.12, and `RCC_APB1LPENR` to 0xB0E64A37, RM0038
  6.3.13, with TIM5LPEN, SPI3LPEN, UART4LPEN and UART5LPEN at 0. The patch applies both to the
  whole family where the SVDs give 0.
- `SPI_CR2` resets to 0 on every category, RM0038 28.5.2, where the STM32L152 SVD gives 0x700.
- `TIM5_ARR` resets to 0xFFFFFFFF, the 32-bit view of RM0038 Table 82. RM0038 17.4.12 shows the
  16-bit view 0xFFFF.
- `CC2S` of `TIM2_CCMR1` and `CC4S` of `TIM2_CCMR2` are two bits wide, RM0038 17.4.7 and 17.4.8,
  where the SVD gives one.
- `TIM2_OR` and `TIM3_OR` reset to 0, RM0038 17.4.19 and 17.4.20.
- `FLASH_WRPR4` resets to 0 with its field named WRP4, on Cat.5 only, RM0038 3.9.9.
- `GPIOx_BRR` reads 0, RM0038 7.4.11.
- `F_SIZE` of DEV_ID 0x436 is 0 for 384 KiB and 1 for 256 KiB, RM0038 31.1.1.
- Cat.2 parts that are not value line carry 20 `RTC_BKPxR`, RM0038 20.2. The
  `stm32l152xba.h` header lists 5.
- Cat.3 on DIE427 and DIE436 has no UART4 and UART5, so the vectors 48 and 49 and the base
  0x40004C00 stay unused, RM0038 Table 50 and the CMSIS headers.
- GPIOF and GPIOG stay in the Cat.6 description with no bonded pin, because the CubeMX part
  files of DIE437 list none.
- The CMSIS device header suffix of a category, xb, xba, xc, xca, xd, xe and xdx, is read from
  the header contents.

These are inferred, not measured on silicon:

- A read past the populated flash inside the 128 KiB window of RM0038 Table 8, the Cat.1 and
  Cat.2 table, is unmapped.
- The 64 KiB hole of Cat.6 between the two 192 KiB banks, 0x08030000 to 0x0803FFFF, is unmapped,
  from RM0038 Table 12 and the CMSIS `FLASH_BANK2_BASE`. Silicon may fault or return a value.
- `SQ24` sits in bits 29 to 25 of `ADC_SQR2`, five bits wide. RM0038 12.15.11 text gives 29
  to 26 and the SVD is used.
- The device unique ID words are fixed values, because RM0038 31.2 documents no layout of the
  wafer and lot fields. The third word sits at offset 0x14, where the STM32U5 and STM32H5 use
  0x08.
- On Cat.1 and Cat.2 `F_SIZE` sits at 0x1FF8004C, inside the factory block 0x1FF80020 to
  0x1FF800FF of RM0038 Table 8.
- `FLASH_WRPR4` also exists on Cat.6 at 0x40023C88, from the CMSIS xdx headers. RM0038 3.9.9
  gives it to Cat.5 only, and whether a Cat.6 access reads zero, faults or returns an option byte
  value is not known.
- The Cat.3 vector positions 46 to 55 follow the vector addresses and the CMSIS header. The
  Position column of RM0038 Table 50 reads 45 to 52.
