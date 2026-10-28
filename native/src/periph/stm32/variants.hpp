#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
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
};

inline constexpr auto u5_limited = std::to_array<std::string_view>({"SPI3"});
/** @brief STM32U5 SPI, RM0456 68.3: SPI3 has an 8-byte FIFO and no `CTSIZE`. */
inline constexpr spi_variant u5_spi{u5_limited};

inline constexpr auto h5_limited = std::to_array<std::string_view>({"SPI4", "SPI5", "SPI6"});
/** @brief STM32H5 SPI, RM0481 Table 645: SPI4 to SPI6 have an 8-byte FIFO and keep TSIZE up to
 *         65535, so `CTSIZE` counts there. RM0481 section 58.11 has no `SPI_AUTOCR`. */
inline constexpr spi_variant h5_spi{h5_limited, true, false};

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
