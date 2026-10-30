#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Prefixes of the per-pin field names of a GPIO port, the pin number appended. */
struct gpio_field_names
{
  /** @brief `GPIOx_MODER` mode field. */
  std::string_view mode = "MODE";
  /** @brief `GPIOx_OTYPER` output type field. */
  std::string_view type = "OT";
  /** @brief `GPIOx_OSPEEDR` speed field. */
  std::string_view speed = "OSPEED";
  /** @brief `GPIOx_PUPDR` pull field. */
  std::string_view pull = "PUPD";
  /** @brief `GPIOx_IDR` input field. */
  std::string_view input = "ID";
  /** @brief `GPIOx_ODR` output field. */
  std::string_view output = "OD";
  /** @brief `GPIOx_AFRL` alternate function field of pins 0 to 7. */
  std::string_view alternate_low = "AFSEL";
  /** @brief `GPIOx_AFRH` alternate function field of pins 8 to 15. */
  std::string_view alternate_high = "AFSEL";
};

/**
 * @brief Differences between the GPIO ports of the families a model row serves.
 *        The model throws at attach when the description disagrees.
 */
struct gpio_variant
{
  /** @brief Port with fewer than 16 pins, 0 for none. */
  char short_port = 0;
  /** @brief Pins of that port. Its registers carry the per-pin fields of these pins only. */
  std::uint8_t short_port_pins = 16;
  /** @brief The port has `GPIOx_SECCFGR`, the TrustZone pin security register. */
  bool secure = true;
  /** @brief The port has `GPIOx_HSLVR`, the high-speed low-voltage register. */
  bool slew = true;
  /** @brief The port has `GPIOx_BRR`, the bit reset register. */
  bool brr = true;
  /** @brief Per-pin field names. */
  gpio_field_names names = {};
};

/** @brief STM32U5 ports with 16 pins, RM0456 13.4.10. */
inline constexpr gpio_variant u5_gpio{};
/** @brief STM32U575 and STM32U585 ports, port I has pins 0 to 7, read back on a NUCLEO-U575ZI-Q:
 *         `GPIOI_MODER` keeps 0x0000FFFF of 0xFFFFFFFF and `GPIOI_AFRH` reads 0, RM0456 13.4.1. */
inline constexpr gpio_variant u575_gpio{.short_port = 'I', .short_port_pins = 8};
/** @brief STM32H56x and H57x ports, port I has pins 0 to 11, read back on an STM32H573:
 *         `GPIOI_MODER` keeps 0x00FFFFFF of 0xFFFFFFFF and `GPIOI_AFRH` holds AFSEL8 to AFSEL11,
 *         as DS14121 Table 17 lists. */
inline constexpr gpio_variant h56x_gpio{.short_port = 'I', .short_port_pins = 12};

/** @brief STM32L1 per-pin field names, RM0038 7.4: MODERy, OSPEEDRy, PUPDRy, IDRy, ODRy, AFRLy
 *         and AFRHy. */
inline constexpr gpio_field_names l1_gpio_names{
    .mode           = "MODER",
    .speed          = "OSPEEDR",
    .pull           = "PUPDR",
    .input          = "IDR",
    .output         = "ODR",
    .alternate_low  = "AFRL",
    .alternate_high = "AFRH",
};
/** @brief STM32L1 Cat.1 to Cat.3 ports, `STM32L1_gpio_cat13`, RM0038 7.4: no `GPIOx_BRR`, which
 *         RM0038 7.4.11 gives to Cat.4, Cat.5 and Cat.6 only, no TrustZone and no HSLV. */
inline constexpr gpio_variant l1_gpio_cat13{
    .secure = false,
    .slew   = false,
    .brr    = false,
    .names  = l1_gpio_names};
/** @brief STM32L1 Cat.4 to Cat.6 ports, `STM32L1_gpio_cat46`, RM0038 7.4 and 7.4.11. */
inline constexpr gpio_variant l1_gpio_cat46{
    .secure = false,
    .slew   = false,
    .brr    = true,
    .names  = l1_gpio_names};

/** @brief NVIC line of the classic EXTI and the EXTI lines first to last that raise it. */
struct exti_line_group
{
  /** @brief First line. */
  std::uint8_t first;
  /** @brief Last line, included. */
  std::uint8_t last;
  /** @brief NVIC line, the IRQ number in the description. The CMSIS device header and the SVD
   *         name some of these lines differently, the number is the same in both. */
  std::uint16_t irq;
};

/** @brief Differences between the classic EXTI controllers the model rows serve. */
struct exti_classic_traits
{
  /** @brief Lines, the width of each register's line field. */
  std::uint8_t lines;
  /** @brief NVIC lines the EXTI raises. A line in no group sets its pending bit and raises
   *         nothing. */
  std::span<exti_line_group const> irqs;
};

/** @brief STM32L1, RM0038 Tables 49 to 51 by NVIC line: EXTI0 to EXTI4 on 6 to 10 one line each,
 *         EXTI9_5 on 23, EXTI15_10 on 40, line 19 the RTC tamper and timestamp line on 2, lines
 *         21 and 22 the comparator wakeup line on 22. PVD, RTC_Alarm, USB_FS_WKUP, RTC_WKUP and
 *         COMP_ACQ, lines 16, 17, 18, 20 and 23, belong to PWR, RTC, USB and COMP in the
 *         description. */
inline constexpr auto l1_exti_irqs = std::to_array<exti_line_group>({
    {0,  0,  6 },
    {1,  1,  7 },
    {2,  2,  8 },
    {3,  3,  9 },
    {4,  4,  10},
    {5,  9,  23},
    {10, 15, 40},
    {19, 19, 2 },
    {21, 22, 22},
});
/** @brief STM32L1 Cat.1 and Cat.2, `STM32L1_exti_23`, RM0038 10.2: 23 lines. */
inline constexpr exti_classic_traits l1_exti_23{.lines = 23, .irqs = l1_exti_irqs};
/** @brief STM32L1 Cat.3 to Cat.6, RM0038 10.2: 24 lines. */
inline constexpr exti_classic_traits l1_exti_24{.lines = 24, .irqs = l1_exti_irqs};

/** @brief One code of a `SYSCFG_EXTICRn` selector field. */
struct exti_port_code
{
  /** @brief Field value. */
  std::uint8_t code;
  /** @brief Port letter. */
  char port;
  /** @brief Lines that take the port at this code, bit n for line n. */
  std::uint16_t lines = 0xFFFF;
};

/** @brief Differences between the classic SYSCFG controllers the model rows serve, by register
 *         role. A role the family lacks is empty. */
struct syscfg_classic_traits
{
  /** @brief Selector codes. A code with no entry, or an entry without the line, selects no
   *         port. */
  std::span<exti_port_code const> ports;
  /** @brief Register of the USB D+ pull-up enable. */
  std::string_view usb_pull_up_register;
  /** @brief Field of the USB D+ pull-up enable. */
  std::string_view usb_pull_up_field;
  /** @brief Register of the memory remap, kept by a SYSCFGRST pulse. */
  std::string_view remap_register;
  /** @brief Registers served as storage the model does not model, with the storage
   *         diagnostics. */
  std::span<std::string_view const> storage_registers;
};

/** @brief RM0038 8.5.3 to 8.5.6, Cat.3 to Cat.6: PA to PE, PH for lines 0 to 2 only, PF and PG. */
inline constexpr auto l1_exti_ports = std::to_array<exti_port_code>({
    {0, 'A'},
    {1, 'B'},
    {2, 'C'},
    {3, 'D'},
    {4, 'E'},
    {5, 'H', 0x0007},
    {6, 'F'},
    {7, 'G'},
});
/** @brief STM32L1 storage registers of SYSCFG: MEMRMP, and PMC until a USB model reads USB_PU. */
inline constexpr auto l1_syscfg_storage = std::to_array<std::string_view>({"MEMRMP", "PMC"});
/** @brief STM32L1 Cat.3 to Cat.6, RM0038 section 8.5. */
inline constexpr syscfg_classic_traits l1_syscfg{
    .ports                = l1_exti_ports,
    .usb_pull_up_register = "PMC",
    .usb_pull_up_field    = "USB_PU",
    .remap_register       = "MEMRMP",
    .storage_registers    = l1_syscfg_storage};
/** @brief STM32L1 Cat.1 and Cat.2, `STM32L1_exti_23`, RM0038 8.5.3: codes 0110 and 0111, PF and
 *         PG, exist on Cat.3 to Cat.6 only. */
inline constexpr syscfg_classic_traits l1_syscfg_cat12{
    .ports                = std::span{l1_exti_ports}.first(6),
    .usb_pull_up_register = "PMC",
    .usb_pull_up_field    = "USB_PU",
    .remap_register       = "MEMRMP",
    .storage_registers    = l1_syscfg_storage};

/** @brief Differences between the STM32L1 FLASH controllers the model rows serve, by the
 *         `STM32L1_flash_<category>` version the category patches set. */
struct flash_l1_traits
{
  /** @brief Highest HCLK in kHz at 0 and 1 wait state in voltage ranges 1, 2 and 3, RM0038
   *         Table 13. */
  std::array<std::array<std::uint32_t, 2>, 3> max_khz;
  /** @brief Whether the part has two banks, FLASH_PECR.PARALLELBANK, RM0038 3.9.2. */
  bool dual_bank = false;
  /** @brief FLASH_WRPRx registers the category has, RM0038 3.9.9. */
  std::uint8_t wrp_registers = 1;
};

/** @brief Table 13 on Cat.2 to Cat.6: Range 3 up to 4.2 MHz at 0 WS and 8 MHz at 1 WS. */
inline constexpr std::array<std::array<std::uint32_t, 2>, 3> l1_flash_wait_khz{
    {{16'000, 32'000}, {8'000, 16'000}, {4'200, 8'000}}
};
/** @brief STM32L1 Cat.1, `STM32L1_flash_cat1`: Range 3 up to 2.1 MHz at 0 WS and 4.2 MHz at
 *         1 WS, RM0038 Table 13. */
inline constexpr flash_l1_traits l1_flash_cat1{
    .max_khz = {{{16'000, 32'000}, {8'000, 16'000}, {2'100, 4'200}}}};
/** @brief STM32L1 Cat.2, `STM32L1_flash_cat2`. */
inline constexpr flash_l1_traits l1_flash_cat2{.max_khz = l1_flash_wait_khz};
/** @brief STM32L1 Cat.3, `STM32L1_flash_cat3`: FLASH_WRPR2. */
inline constexpr flash_l1_traits l1_flash_cat3{.max_khz = l1_flash_wait_khz, .wrp_registers = 2};
/** @brief STM32L1 Cat.4 and Cat.6, `STM32L1_flash_cat46`: two banks, FLASH_WRPR2 and
 *         FLASH_WRPR3. */
inline constexpr flash_l1_traits l1_flash_cat46{
    .max_khz       = l1_flash_wait_khz,
    .dual_bank     = true,
    .wrp_registers = 3};
/** @brief STM32L1 Cat.5, `STM32L1_flash_cat5`: two banks, FLASH_WRPR2 to FLASH_WRPR4. */
inline constexpr flash_l1_traits l1_flash_cat5{
    .max_khz       = l1_flash_wait_khz,
    .dual_bank     = true,
    .wrp_registers = 4};

/** @brief Differences between the STM32U5 FLASH controllers the model rows serve. */
struct flash_variant
{
  /** @brief Width of the SECWM, HDP and WRP page fields, RM0456 7.9.17. */
  std::uint8_t watermark_bits = 7;
};

/** @brief STM32U535 and STM32U545, 5 bits. */
inline constexpr flash_variant flash_5bit{.watermark_bits = 5};
/** @brief STM32U59x, U5Ax, U5Fx and U5Gx, 8 bits. */
inline constexpr flash_variant flash_8bit{.watermark_bits = 8};

/** @brief Differences between the STM32H5 HASH processors the model rows serve, RM0481 40.3.
 *         A code outside the variant's set reads 0000 after a CR write and the hash runs SHA-1,
 *         as read back on an STM32H573. */
struct hash_variant
{
  /** @brief ALGO also keeps the SHA-3 and SHAKE codes 0100 to 1001. */
  bool sha3 = false;
};

/** @brief STM32H52x, H53x, H56x and H57x, SHA-1 and SHA-2 only, RM0481 Table 397: ALGO keeps
 *         0000, 0010, 0011 and 1100 to 1111. */
inline constexpr hash_variant hash_h5_sha2{};
/** @brief STM32H543 and H553, SHA-3 and SHAKE too, RM0481 Table 397. */
inline constexpr hash_variant hash_h5_sha3{.sha3 = true};

/** @brief Differences between the I2C peripherals the model rows serve. */
struct i2c_variant
{
  /** @brief The peripheral has the `I2C_AUTOCR` register and its trigger and DMA fields. */
  bool autocr = true;
};

/** @brief STM32U5 I2C, RM0456 65.7.14. */
inline constexpr i2c_variant u5_i2c{};
/** @brief STM32H5 I2C, RM0481 has no `I2C_AUTOCR`. */
inline constexpr i2c_variant h5_i2c{.autocr = false};

/** @brief Differences between the USART, UART and LPUART peripherals the model rows serve. */
struct usart_variant
{
  /** @brief The peripheral has the `CR1_enabled` and `ISR_enabled` register views and no `CR1`
   *         and `ISR`. */
  bool mode_views = true;
  /** @brief The peripheral has the `FIFOEN` control bit, otherwise it holds one data word. */
  bool fifo = true;
  /** @brief Name of the IRQ line, empty for `<instance>_IRQn`. */
  std::string_view irq_name = {};
};

/** @brief STM32U5 USART and UART, RM0456 66.8, and every STM32H5 USART and UART, RM0481 56.8. */
inline constexpr usart_variant with_views_usart{};
/** @brief STM32U5 LPUART, RM0456 67.7, one `CR1` and one `ISR`. */
inline constexpr usart_variant u5_lpuart{.mode_views = false};
/** @brief STM32H5 LPUART, RM0481 57.7: the register views of the USART, and the CMSIS header
 *         names its line `LPUART1_IRQn` for the peripheral `LPUART`. */
inline constexpr usart_variant h5_lpuart{.irq_name = "LPUART1_IRQn"};

/** @brief Differences between the RTC and TAMP peripherals the model rows serve. */
struct rtc_variant
{
  /** @brief Name of the privilege register the security write handler binds. */
  std::string_view privilege_register = "PRIVCR";
};

/** @brief STM32U5 RTC and TAMP, RM0456 63.6.14 and 64.6.7. */
inline constexpr rtc_variant u5_rtc{};
/** @brief STM32H5 RTC and TAMP, RM0481 51.6.8 and 52.7.10. */
inline constexpr rtc_variant h5_rtc{.privilege_register = "PRIVCFGR"};

/** @brief Differences between the SAI peripherals the model rows serve. */
struct sai_variant
{
  /** @brief Name of the block enable field of `xCR1`. */
  std::string_view enable = "SAIAEN";
};

/** @brief STM32U5 SAI, RM0456 69.6.2. */
inline constexpr sai_variant u5_sai{};
/** @brief STM32H5 SAI, RM0481 59.6.2. */
inline constexpr sai_variant h5_sai{.enable = "SAIEN"};

/** @brief One internal trigger input of a timer's slave mode controller. */
struct trigger_entry
{
  /** @brief SMCR.TS value that selects the input. */
  std::uint32_t select;
  /** @brief Timer whose `<name>_trgo` the input carries, or the unmodelled source, such as
   *         `tim13_oc1`, which no model produces. */
  std::string_view source;
};

/** @brief Differences between the timer peripherals the model rows serve. */
struct tim_variant
{
  /** @brief Internal trigger inputs by TS value. The input a timer would take from itself is
   *         reserved, and TS values with no entry reach nothing. */
  std::span<trigger_entry const> triggers;
  /** @brief Instances whose CR2.MMS has 4 bits, bit 3 in field `MMS_1`. The other timers with an
   *         MMS field have 3 bits. */
  std::span<std::string_view const> wide_master;
  /** @brief Instances with no master mode, so no CR2.MMS field. Every other timer has one. */
  std::span<std::string_view const> no_master;
};

/** @brief RM0456 54.6.2 and 55.5.2, RM0481 43.6.2 and 44.5.2: MMS[3:0] on TIM1, TIM8 and TIM2 to
 *         TIM5. RM0456 56.7.2, 57.4.2 and RM0481 45.4.2, 46.7.2, 47.7.2: MMS[2:0] on the others. */
inline constexpr auto wide_master_timers =
    std::to_array<std::string_view>({"TIM1", "TIM2", "TIM3", "TIM4", "TIM5", "TIM8"});

/** @brief TS 0 to 3 are ITR0 to ITR3, 8 to 10 are ITR4 to ITR6, RM0456 54.6.3 and 55.5.3 TIMx_SMCR.
 */
inline constexpr auto u5_triggers = std::to_array<trigger_entry>({
    {0,  "tim1" },
    {1,  "tim2" },
    {2,  "tim3" },
    {3,  "tim4" },
    {8,  "tim5" },
    {9,  "tim8" },
    {10, "tim15"},
});
/** @brief RM0456 56.8.2: TIM16 and TIM17 have no master mode. */
inline constexpr auto u5_no_master = std::to_array<std::string_view>({"TIM16", "TIM17"});
/** @brief STM32U5 timers. */
inline constexpr tim_variant u5_tim{u5_triggers, wide_master_timers, u5_no_master};

/**
 * @brief TS 0 to 3 are ITR0 to ITR3, 8 to 15 are ITR4 to ITR11 and 16 is ITR12, RM0481 43.6.3
 *        and 44.5.3 TIMx_SMCR. RM0481 Tables 456, 480, 502 and 514 connect the same source to the
 *        same ITR on TIM1, TIM2 to TIM5, TIM8, TIM12 and TIM15. ITR12 is USBSOF on TIM2 and TIM5
 *        and reserved on the other six. Sources with no model: the OC1 outputs of TIM13, TIM14,
 *        TIM16 and TIM17 and USBSOF.
 */
inline constexpr auto h5_triggers = std::to_array<trigger_entry>({
    {0,  "tim1"     },
    {1,  "tim2"     },
    {2,  "tim3"     },
    {3,  "tim4"     },
    {8,  "tim5"     },
    {9,  "tim8"     },
    {10, "tim12"    },
    {11, "tim13_oc1"},
    {12, "tim14_oc1"},
    {13, "tim15"    },
    {14, "tim16_oc1"},
    {15, "tim17_oc1"},
    {16, "usbsof"   },
});
/** @brief RM0481 46.8.13: TIM13 and TIM14 have no CR2. RM0481 47.8.2: TIM16 and TIM17 have no
 *         master mode. */
inline constexpr auto h5_no_master =
    std::to_array<std::string_view>({"TIM13", "TIM14", "TIM16", "TIM17"});
/** @brief STM32H5 timers. */
inline constexpr tim_variant h5_tim{h5_triggers, wide_master_timers, h5_no_master};

/** @brief Differences between the SPI peripherals the model rows serve. */
struct spi_variant
{
  /** @brief Instances with an 8-byte FIFO, the others hold 16 bytes. */
  std::span<std::string_view const> limited;
  /** @brief The limited instances have the `CTSIZE` remaining-frames counter. */
  bool limited_counter = false;
  /** @brief The peripheral has the `SPI_AUTOCR` register and its trigger field. */
  bool autocr = true;
  /** @brief `SPI_CFG1` TXDMAEN and RXDMAEN stay writable while SPE = 1, the rest of the register
   * is write-protected then. Otherwise the whole register is. */
  bool dma_enables_writable = true;
};

inline constexpr auto u5_limited = std::to_array<std::string_view>({"SPI3"});
/** @brief STM32U5 SPI, RM0456 68.3: SPI3 has an 8-byte FIFO and no `CTSIZE`. RM0456 68.8.3:
 *         `SPI_CFG1` is write-protected while SPE = 1 except TXDMAEN and RXDMAEN. */
inline constexpr spi_variant u5_spi{u5_limited};

inline constexpr auto h5_limited = std::to_array<std::string_view>({"SPI4", "SPI5", "SPI6"});
/** @brief STM32H5 SPI, RM0481 Table 645: SPI4 to SPI6 have an 8-byte FIFO and keep TSIZE up to
 *         65535, so `CTSIZE` counts there. RM0481 section 58.11 has no `SPI_AUTOCR`. RM0481
 *         58.11.3: the whole `SPI_CFG1` is write-protected while SPE = 1. */
inline constexpr spi_variant h5_spi{
    .limited              = h5_limited,
    .limited_counter      = true,
    .autocr               = false,
    .dma_enables_writable = false};

/** @brief A TZSC `<STEM>SEC` or TZIC `<STEM>IE` stem whose peripheral has another name. */
struct gtzc_rename
{
  /** @brief Stem of the field. */
  std::string_view stem;
  /** @brief Peripheral the stem names. */
  std::string_view peripheral;
};

/** @brief A peripheral that one TZSC or TZIC bit covers together with another. */
struct gtzc_companion
{
  /** @brief Peripheral the stem names, after the renames. */
  std::string_view primary;
  /** @brief Peripheral the same bit also covers. */
  std::string_view also;
};

/** @brief Differences between the GTZC TZSC and TZIC field stems of the families a model row
 *         serves: the stems that name no peripheral of the same name. */
struct gtzc_variant
{
  /** @brief Stems naming a peripheral of another name. */
  std::span<gtzc_rename const> renames;
  /** @brief Peripherals one bit covers with the peripheral it names. */
  std::span<gtzc_companion const> companions;
};

inline constexpr auto u5_gtzc_renames    = std::to_array<gtzc_rename>({
    {"USART4",       "UART4"   },
    {"ICACHE",       "ICache"  },
    {"ICACHE_REG",   "ICache"  },
    {"DCACHE_REG",   "DCACHE"  },
    {"OTGFS",        "OTG_FS"  },
    {"FSMC",         "FMC"     },
    {"FSMC_REG",     "FMC"     },
    {"OCTOSPI1_REG", "OCTOSPI1"},
    {"OCTOSPI2_REG", "OCTOSPI2"},
    {"FLASH_REG",    "FLASH"   },
});
inline constexpr auto u5_gtzc_companions = std::to_array<gtzc_companion>({
    {"DCMI",     "PSSI"      },
    {"ADC1",     "ADC12"     },
    {"FDCAN1",   "FDCAN1_RAM"},
    {"SDMMC1",   "DLYBSD1"   },
    {"SDMMC2",   "DLYBSD2"   },
    {"OCTOSPI1", "DLYBOS1"   },
    {"OCTOSPI2", "DLYBOS2"   },
});
/** @brief STM32U5 GTZC1 and GTZC2, RM0456 5.7.4 to 5.7.6 and 5.8: the SVD instance names of
 *         UART4, the instruction cache, the OTG, the FMC and the register blocks, the bits
 *         that RM0456 Table 5 gives to two blocks, and each delay block with the peripheral
 *         RM0456 Table 297 associates it with. */
inline constexpr gtzc_variant u5_gtzc{u5_gtzc_renames, u5_gtzc_companions};

inline constexpr auto h5_gtzc_renames    = std::to_array<gtzc_rename>({
    {"ADC12",     "ADC1"   },
    {"DAC1",      "DAC"    },
    {"I3C1",      "I3C"    },
    {"LPUART1",   "LPUART" },
    {"OCTOSPI1",  "OCTOSPI"},
    {"UCPD",      "UCPD1"  },
    {"FLASH_REG", "FLASH"  },
});
inline constexpr auto h5_gtzc_companions = std::to_array<gtzc_companion>({
    {"DCMI",    "PSSI"   },
    {"ADC1",    "ADC2"   },
    {"ADC1",    "ADCC"   },
    {"SDMMC1",  "DLYBSD1"},
    {"SDMMC2",  "DLYBSD2"},
    {"OCTOSPI", "DLYBOS1"},
});
/** @brief STM32H562, H563 and H573 GTZC1, RM0481 5.7.4 to 5.7.6 and 5.8: the SVD names
 *         `DAC`, `I3C`, `LPUART`, `OCTOSPI` and `UCPD1` of the stems `DAC1`, `I3C1`,
 *         `LPUART1`, `OCTOSPI1` and `UCPD`, and `ADC12SEC` covering ADC1, ADC2 and their common
 *         registers `ADCC`, RM0481 Table 3, and each delay block with the peripheral RM0481
 *         Table 275 associates it with. `HDMICEC` names no peripheral of the SVD. */
inline constexpr gtzc_variant h5_gtzc{h5_gtzc_renames, h5_gtzc_companions};
}
