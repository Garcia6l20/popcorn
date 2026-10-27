#pragma once

#include "rcc_graph.hpp"

#include <popcorn/core/types.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace popcorn::periph::stm32
{
namespace
{
namespace u5
{
enum class source : std::uint8_t
{
  msis,
  msik,
  hsi16,
  hsi48,
  shsi,
  hse,
  lsi,
  lse,
  pll1,
  pll2,
  pll3,
};

enum class signal : std::uint8_t
{
  none,
  msis,
  msik,
  hsi16,
  hsi48,
  hsi48_half,
  hse,
  lsi,
  lse,
  sysclk,
  hclk,
  pclk1,
  pclk2,
  pclk3,
  pll1_p,
  pll1_q,
  pll1_r,
  pll2_p,
  pll2_q,
  pll2_r,
  pll3_p,
  pll3_q,
  pll3_r,
  iclk,
  rtc,
};

using enum signal;

constexpr std::array<signal, 8> uart_inputs_1{pclk1, sysclk, hsi16, lse};
constexpr std::array<signal, 8> i2c_inputs_1{pclk1, sysclk, hsi16, msik};
constexpr std::array<signal, 8> low_power_inputs{msik, lsi, hsi16, lse};
constexpr std::array<signal, 8> audio_inputs{hclk, pll1_p, pll3_q, none, msik};
constexpr std::array<signal, 8> sai_inputs{pll2_p, pll3_p, pll1_p, none, hsi16};
constexpr std::array<signal, 8> pclk3_inputs{pclk3, sysclk, hsi16, msik};

constexpr std::array<mux<signal>, 28> muxes{
    {
     {"RCC_CCIPR1", "USART1SEL", {pclk2, sysclk, hsi16, lse}, {{{"USART1", "USART1EN"}}}},
     {"RCC_CCIPR1", "USART2SEL", uart_inputs_1, {{{"USART2", "USART2EN"}}}},
     {"RCC_CCIPR1", "USART3SEL", uart_inputs_1, {{{"USART3", "USART3EN"}}}},
     {"RCC_CCIPR1", "UART4SEL", uart_inputs_1, {{{"UART4", "UART4EN"}}}},
     {"RCC_CCIPR1", "UART5SEL", uart_inputs_1, {{{"UART5", "UART5EN"}}}},
     {"RCC_CCIPR1", "I2C1SEL", i2c_inputs_1, {{{"I2C1", "I2C1EN"}}}},
     {"RCC_CCIPR1", "I2C2SEL", i2c_inputs_1, {{{"I2C2", "I2C2EN"}}}},
     {"RCC_CCIPR1", "I2C4SEL", i2c_inputs_1, {{{"I2C4", "I2C4EN"}}}},
     {"RCC_CCIPR1", "SPI2SEL", i2c_inputs_1, {{{"SPI2", "SPI2EN"}}}},
     {"RCC_CCIPR1", "LPTIM2SEL", {pclk1, lsi, hsi16, lse}, {{{"LPTIM2", "LPTIM2EN"}}}},
     {"RCC_CCIPR1", "SPI1SEL", {pclk2, sysclk, hsi16, msik}, {{{"SPI1", "SPI1EN"}}}},
     {"RCC_CCIPR1", "FDCAN1SEL", {hse, pll1_q, pll2_p, none}, {{{"FDCAN1", "FDCAN1EN"}}}},
     {"RCC_CCIPR2", "MDF1SEL", audio_inputs, {{{"MDF1", "MDF1EN"}}}},
     {"RCC_CCIPR2", "SAI1SEL", sai_inputs, {{{"SAI1", "SAI1EN"}}}},
     {"RCC_CCIPR2", "SAI2SEL", sai_inputs, {{{"SAI2", "SAI2EN"}}}},
     {"RCC_CCIPR2", "RNGSEL", {hsi48, hsi48_half, hsi16, none}, {{{"RNG", "RNGEN"}}}},
     {"RCC_CCIPR2",
         "SDMMCSEL",
         {iclk, pll1_p},
         {{{"SDMMC1", "SDMMC1EN"}, {"SDMMC2", "SDMMC2EN"}}}},
     {"RCC_CCIPR2", "USART6SEL", uart_inputs_1, {{{"USART6", "USART6EN"}}}},
     {"RCC_CCIPR2",
         "OCTOSPISEL",
         {sysclk, msik, pll1_q, pll2_q},
         {{{"OCTOSPI1", "OCTOSPI1EN"}, {"OCTOSPI2", "OCTOSPI2EN"}}}},
     {"RCC_CCIPR2", "I2C5SEL", i2c_inputs_1, {{{"I2C5", "I2C5EN"}}}},
     {"RCC_CCIPR2", "I2C6SEL", i2c_inputs_1, {{{"I2C6", "I2C6EN"}}}},
     {"RCC_CCIPR3",
         "LPUART1SEL",
         {pclk3, sysclk, hsi16, lse, msik},
         {{{"LPUART1", "LPUART1EN"}}}},
     {"RCC_CCIPR3", "SPI3SEL", pclk3_inputs, {{{"SPI3", "SPI3EN"}}}},
     {"RCC_CCIPR3", "I2C3SEL", pclk3_inputs, {{{"I2C3", "I2C3EN"}}}},
     {"RCC_CCIPR3",
         "LPTIM34SEL",
         low_power_inputs,
         {{{"LPTIM3", "LPTIM3EN"}, {"LPTIM4", "LPTIM4EN"}}}},
     {"RCC_CCIPR3", "LPTIM1SEL", low_power_inputs, {{{"LPTIM1", "LPTIM1EN"}}}},
     {"RCC_CCIPR3",
         "ADCDACSEL",
         {hclk, sysclk, pll2_r, hse, hsi16, msik},
         {{{"ADC1", "ADC12EN"}, {"ADC2", "ADC12EN"}, {"ADC4", "ADC4EN"}, {"DAC1", "DAC1EN"}}}},
     {"RCC_CCIPR3", "ADF1SEL", audio_inputs, {{{"ADF1", "ADF1EN"}}}},
     }
};

constexpr std::array<timer_names<signal>, 11> timers{
    {
     {"TIM1", "TIM1EN", pclk2},
     {"TIM2", "TIM2EN", pclk1},
     {"TIM3", "TIM3EN", pclk1},
     {"TIM4", "TIM4EN", pclk1},
     {"TIM5", "TIM5EN", pclk1},
     {"TIM6", "TIM6EN", pclk1},
     {"TIM7", "TIM7EN", pclk1},
     {"TIM8", "TIM8EN", pclk2},
     {"TIM15", "TIM15EN", pclk2},
     {"TIM16", "TIM16EN", pclk2},
     {"TIM17", "TIM17EN", pclk2},
     }
};

constexpr std::array<std::string_view, 8> enable_registers{
    "RCC_AHB1ENR",  "RCC_AHB2ENR1", "RCC_AHB2ENR2", "RCC_AHB3ENR",
    "RCC_APB1ENR1", "RCC_APB1ENR2", "RCC_APB2ENR",  "RCC_APB3ENR"};

constexpr std::array<std::uint64_t, 16> msi_hz{
    48'000'000, 24'000'000, 16'000'000, 12'000'000, 4'000'000, 2'000'000, 1'330'000, 1'000'000,
    3'072'000,  1'536'000,  1'024'000,  768'000,    400'000,   200'000,   133'000,   100'000};

constexpr std::uint64_t hsi16_hz = 16'000'000;
constexpr std::uint64_t hsi48_hz = 48'000'000;
constexpr std::uint64_t lsi_hz   = 32'000;

auto msis_rate(rate_inputs const& in) -> std::uint64_t
{
  using enum rcc_role;
  return msi_hz[in.read(in.read(msi_select) != 0 ? msis_range : msis_standby_range)];
}

auto msik_rate(rate_inputs const& in) -> std::uint64_t
{
  using enum rcc_role;
  return msi_hz[in.read(in.read(msi_select) != 0 ? msik_range : msik_standby_range)];
}

auto lsi_rate(rate_inputs const& in) -> std::uint64_t
{
  return in.read(rcc_role::lsi_prediv) != 0 ? lsi_hz / 128 : lsi_hz;
}

constexpr std::array<oscillator_names<signal>, 11> oscillators{
    {
     {"RCC_CR", "MSISON", "MSISRDY", "MSISRDYF", "MSISRDYIE", msis, &msis_rate},
     {"RCC_CR", "MSIKON", "MSIKRDY", "MSIKRDYF", "MSIKRDYIE", msik, &msik_rate},
     {"RCC_CR", "HSION", "HSIRDY", "HSIRDYF", "HSIRDYIE", hsi16, &fixed_rate<hsi16_hz>},
     {"RCC_CR", "HSI48ON", "HSI48RDY", "HSI48RDYF", "HSI48RDYIE", hsi48, &fixed_rate<hsi48_hz>},
     {"RCC_CR", "SHSION", "SHSIRDY", "SHSIRDYF", "SHSIRDYIE"},
     {"RCC_CR", "HSEON", "HSERDY", "HSERDYF", "HSERDYIE", hse, &hse_rate},
     {"RCC_BDCR", "LSION", "LSIRDY", "LSIRDYF", "LSIRDYIE", lsi, &lsi_rate},
     {"RCC_BDCR", "LSEON", "LSERDY", "LSERDYF", "LSERDYIE", lse, &lse_rate},
     {"RCC_CR", "PLL1ON", "PLL1RDY", "PLL1RDYF", "PLL1RDYIE"},
     {"RCC_CR", "PLL2ON", "PLL2RDY", "PLL2RDYF", "PLL2RDYIE"},
     {"RCC_CR", "PLL3ON", "PLL3RDY", "PLL3RDYF", "PLL3RDYIE"},
     }
};

constexpr auto names = std::to_array<named_field<rcc_role>>({
    {rcc_role::css_on,             {"RCC_CR", "CSSON"}        },
    {rcc_role::msi_select,         {"RCC_ICSCR1", "MSIRGSEL"} },
    {rcc_role::msis_range,         {"RCC_ICSCR1", "MSISRANGE"}},
    {rcc_role::msik_range,         {"RCC_ICSCR1", "MSIKRANGE"}},
    {rcc_role::msis_standby_range, {"RCC_CSR", "MSISSRANGE"}  },
    {rcc_role::msik_standby_range, {"RCC_CSR", "MSIKSRANGE"}  },
    {rcc_role::sw,                 {"RCC_CFGR1", "SW"}        },
    {rcc_role::sws,                {"RCC_CFGR1", "SWS"}       },
    {rcc_role::hpre,               {"RCC_CFGR2", "HPRE"}      },
    {rcc_role::ppre1,              {"RCC_CFGR2", "PPRE1"}     },
    {rcc_role::ppre2,              {"RCC_CFGR2", "PPRE2"}     },
    {rcc_role::ppre3,              {"RCC_CFGR3", "PPRE3"}     },
    {rcc_role::rtc_select,         {"RCC_BDCR", "RTCSEL"}     },
    {rcc_role::rtc_enable,         {"RCC_BDCR", "RTCEN"}      },
    {rcc_role::lsi_prediv,         {"RCC_BDCR", "LSIPREDIV"}  },
    {rcc_role::backup_reset,       {"RCC_BDCR", "BDRST"}      },
    {rcc_role::remove_flags,       {"RCC_CSR", "RMVF"}        },
    {rcc_role::hsi_divider,        {}                         },
    {rcc_role::hsi_divider_flag,   {}                         },
    {rcc_role::timer_prescaler,    {}                         },
    {rcc_role::rtc_prescaler,      {}                         },
});

static_assert(names_every_role(names));

constexpr std::array<signal_mux<signal>, 1> signal_muxes{
    {
     {"RCC_CCIPR1", "ICLKSEL", {hsi48, pll2_q, pll1_q, msik}, iclk},
     }
};

constexpr std::array<divided_signal<signal>, 1> divided{
    {
     {hsi48_half, hsi48, 2},
     }
};

constexpr std::array<signal, 1> internal_signals{hsi48_half};

constexpr std::array<rtc_input<signal>, 4> rtc_sources{
    {{none, &divide_by<1>}, {lse, &divide_by<1>}, {lsi, &divide_by<1>}, {hse, &divide_by<32>}}
};

constexpr std::array<source, 4> pll_inputs{source::pll1, source::msis, source::hsi16, source::hse};
constexpr std::array<signal, 4> pll_sources{none, msis, hsi16, hse};
constexpr std::array<std::optional<source>, 4> system_inputs{
    source::msis, source::hsi16, source::hse, source::pll1};
constexpr std::array<signal, 4> system_sources{msis, hsi16, hse, pll1_r};

constexpr std::array<bus_node<signal>, 3> buses{
    {{pclk1, rcc_role::ppre1}, {pclk2, rcc_role::ppre2}, {pclk3, rcc_role::ppre3}}
};

constexpr std::array<pll_node<signal>, 3> pll_nodes{
    {
     {.config        = "RCC_PLL1CFGR",
         .dividers      = "RCC_PLL1DIVR",
         .fraction      = "RCC_PLL1FRACR",
         .src           = "PLL1SRC",
         .range         = "PLL1RGE",
         .m             = "PLL1M",
         .fracen        = "PLL1FRACEN",
         .n             = "PLL1N",
         .frac          = "PLL1FRACN",
         .divider       = {"PLL1P", "PLL1Q", "PLL1R"},
         .output_enable = {"PLL1PEN", "PLL1QEN", "PLL1REN"},
         .out           = {pll1_p, pll1_q, pll1_r},
         .outputs       = 3,
         .m_decode      = &plus_one,
         .n_decode      = &plus_one,
         .output_decode = {&plus_one, &plus_one, &plus_one}},
     {.config        = "RCC_PLL2CFGR",
         .dividers      = "RCC_PLL2DIVR",
         .fraction      = "RCC_PLL2FRACR",
         .src           = "PLL2SRC",
         .range         = "PLL2RGE",
         .m             = "PLL2M",
         .fracen        = "PLL2FRACEN",
         .n             = "PLL2N",
         .frac          = "PLL2FRACN",
         .divider       = {"PLL2P", "PLL2Q", "PLL2R"},
         .output_enable = {"PLL2PEN", "PLL2QEN", "PLL2REN"},
         .out           = {pll2_p, pll2_q, pll2_r},
         .outputs       = 3,
         .m_decode      = &plus_one,
         .n_decode      = &plus_one,
         .output_decode = {&plus_one, &plus_one, &plus_one}},
     {.config        = "RCC_PLL3CFGR",
         .dividers      = "RCC_PLL3DIVR",
         .fraction      = "RCC_PLL3FRACR",
         .src           = "PLL3SRC",
         .range         = "PLL3RGE",
         .m             = "PLL3M",
         .fracen        = "PLL3FRACEN",
         .n             = "PLL3N",
         .frac          = "PLL3FRACN",
         .divider       = {"PLL3P", "PLL3Q", "PLL3R"},
         .output_enable = {"PLL3PEN", "PLL3QEN", "PLL3REN"},
         .out           = {pll3_p, pll3_q, pll3_r},
         .outputs       = 3,
         .m_decode      = &plus_one,
         .n_decode      = &plus_one,
         .output_decode = {&plus_one, &plus_one, &plus_one}},
     }
};
}

namespace u5
{
constexpr std::array<std::string_view, 7> reset_flags{"BORRSTF",  "PINRSTF",  "SFTRSTF", "IWDGRSTF",
                                                      "WWDGRSTF", "LPWRRSTF", "OBLRSTF"};

constexpr auto rcc_secured = std::to_array<secured_group>({
    {"HSISEC",    "RCC_CR",        "HSION HSIKERON HSIRDY"                                               },
    {"HSISEC",    "RCC_ICSCR3",    "HSICAL HSITRIM"                                                      },
    {"HSISEC",    "RCC_CIER",      "HSIRDYIE"                                                            },
    {"HSISEC",    "RCC_CIFR",      "HSIRDYF"                                                             },
    {"HSISEC",    "RCC_CICR",      "HSIRDYC"                                                             },
    {"HSESEC",    "RCC_CR",        "HSEON HSERDY HSEBYP CSSON HSEEXT"                                    },
    {"HSESEC",    "RCC_CIER",      "HSERDYIE"                                                            },
    {"HSESEC",    "RCC_CIFR",      "HSERDYF CSSF"                                                        },
    {"HSESEC",    "RCC_CICR",      "HSERDYC CSSC"                                                        },
    {"MSISEC",    "RCC_CR",        "MSISON MSIKERON MSISRDY MSIPLLEN MSIKON MSIKRDY MSIPLLSEL MSIPLLFAST"},
    {"MSISEC",    "RCC_ICSCR1",
     "MSISRANGE MSIKRANGE MSIRGSEL MSIBIAS MSICAL0 MSICAL1 MSICAL2 MSICAL3"                              },
    {"MSISEC",    "RCC_ICSCR2",    "MSITRIM0 MSITRIM1 MSITRIM2 MSITRIM3"                                 },
    {"MSISEC",    "RCC_CIER",      "MSISRDYIE MSIKRDYIE"                                                 },
    {"MSISEC",    "RCC_CIFR",      "MSISRDYF MSIKRDYF"                                                   },
    {"MSISEC",    "RCC_CICR",      "MSISRDYC MSIKRDYC"                                                   },
    {"MSISEC",    "RCC_CSR",       "MSISSRANGE MSIKSRANGE"                                               },
    {"LSISEC",    "RCC_BDCR",      "LSION LSIRDY LSIPREDIV LSCOSEL LSCOEN"                               },
    {"LSISEC",    "RCC_CIER",      "LSIRDYIE"                                                            },
    {"LSISEC",    "RCC_CIFR",      "LSIRDYF"                                                             },
    {"LSISEC",    "RCC_CICR",      "LSIRDYC"                                                             },
    {"LSESEC",    "RCC_BDCR",
     "LSECSSON LSECSSD LSEDRV LSEBYP LSERDY LSEON LSEGFON LSESYSRDY LSESYSEN LSCOSEL LSCOEN"             },
    {"LSESEC",    "RCC_CIER",      "LSERDYIE"                                                            },
    {"LSESEC",    "RCC_CIFR",      "LSERDYF"                                                             },
    {"LSESEC",    "RCC_CICR",      "LSERDYC"                                                             },
    {"SYSCLKSEC", "RCC_CFGR1",     "SW SWS STOPWUCK STOPKERWUCK MCOSEL MCOPRE"                           },
    {"SYSCLKSEC", "RCC_CCIPR1",    "SYSTICKSEL"                                                          },
    {"PRESCSEC",  "RCC_CFGR2",     "HPRE PPRE1 PPRE2"                                                    },
    {"PRESCSEC",  "RCC_CFGR3",     "PPRE3"                                                               },
    {"PLL1SEC",   "RCC_PLL1CFGR",
     "PLL1SRC PLL1RGE PLL1FRACEN PLL1M PLL1MBOOST PLL1PEN PLL1QEN PLL1REN"                               },
    {"PLL1SEC",   "RCC_PLL1DIVR",  "PLL1N PLL1P PLL1Q PLL1R"                                             },
    {"PLL1SEC",   "RCC_PLL1FRACR", "PLL1FRACN"                                                           },
    {"PLL1SEC",   "RCC_CR",        "PLL1RDY PLL1ON"                                                      },
    {"PLL1SEC",   "RCC_CIER",      "PLL1RDYIE"                                                           },
    {"PLL1SEC",   "RCC_CIFR",      "PLL1RDYF"                                                            },
    {"PLL1SEC",   "RCC_CICR",      "PLL1RDYC"                                                            },
    {"PLL2SEC",   "RCC_PLL2CFGR",  "PLL2SRC PLL2RGE PLL2FRACEN PLL2M PLL2PEN PLL2QEN PLL2REN"            },
    {"PLL2SEC",   "RCC_PLL2DIVR",  "PLL2N PLL2P PLL2Q PLL2R"                                             },
    {"PLL2SEC",   "RCC_PLL2FRACR", "PLL2FRACN"                                                           },
    {"PLL2SEC",   "RCC_CR",        "PLL2RDY PLL2ON"                                                      },
    {"PLL2SEC",   "RCC_CIER",      "PLL2RDYIE"                                                           },
    {"PLL2SEC",   "RCC_CIFR",      "PLL2RDYF"                                                            },
    {"PLL2SEC",   "RCC_CICR",      "PLL2RDYC"                                                            },
    {"PLL3SEC",   "RCC_PLL3CFGR",  "PLL3SRC PLL3RGE PLL3FRACEN PLL3M PLL3PEN PLL3QEN PLL3REN"            },
    {"PLL3SEC",   "RCC_PLL3DIVR",  "PLL3N PLL3P PLL3Q PLL3R"                                             },
    {"PLL3SEC",   "RCC_PLL3FRACR", "PLL3FRACN"                                                           },
    {"PLL3SEC",   "RCC_CR",        "PLL3RDY PLL3ON"                                                      },
    {"PLL3SEC",   "RCC_CIER",      "PLL3RDYIE"                                                           },
    {"PLL3SEC",   "RCC_CIFR",      "PLL3RDYF"                                                            },
    {"PLL3SEC",   "RCC_CICR",      "PLL3RDYC"                                                            },
    {"HSI48SEC",  "RCC_CR",        "HSI48ON HSI48RDY"                                                    },
    {"HSI48SEC",  "RCC_CRRCR",     "HSI48CAL"                                                            },
    {"HSI48SEC",  "RCC_CIER",      "HSI48RDYIE"                                                          },
    {"HSI48SEC",  "RCC_CIFR",      "HSI48RDYF"                                                           },
    {"HSI48SEC",  "RCC_CICR",      "HSI48RDYC"                                                           },
    {"ICLKSEC",   "RCC_CCIPR1",    "ICLKSEL"                                                             },
    {"RMVFSEC",   "RCC_CSR",       "RMVF"                                                                },
});

constexpr auto rcc_unit_registers = std::to_array<std::string_view>({
    "RCC_AHB1RSTR",   "RCC_AHB2RSTR1",  "RCC_AHB2RSTR2",  "RCC_AHB3RSTR",   "RCC_APB1RSTR1",
    "RCC_APB1RSTR2",  "RCC_APB2RSTR",   "RCC_APB3RSTR",   "RCC_AHB1ENR",    "RCC_AHB2ENR1",
    "RCC_AHB2ENR2",   "RCC_AHB3ENR",    "RCC_APB1ENR1",   "RCC_APB1ENR2",   "RCC_APB2ENR",
    "RCC_APB3ENR",    "RCC_AHB1SMENR",  "RCC_AHB2SMENR1", "RCC_AHB2SMENR2", "RCC_AHB3SMENR",
    "RCC_APB1SMENR1", "RCC_APB1SMENR2", "RCC_APB2SMENR",  "RCC_APB3SMENR",  "RCC_SRDAMR",
    "RCC_CCIPR1",     "RCC_CCIPR2",     "RCC_CCIPR3",
});

constexpr auto rcc_unit_aliases = std::to_array<unit_alias>({
    {"DCMI_PSSI", "DCMI"   },
    {"FSMC",      "FMC"    },
    {"OTG",       "OTG_FS" },
    {"ICACHE",    "ICache" },
    {"DCACHE1",   "DCACHE" },
    {"VREF",      "VREFBUF"},
    {"RTCAPB",    "RTC"    },
});
}

/**
 * @brief Clock tree of the STM32U5 RCC, RM0456 section 11: its sources and signals, the node
 *        tables the RCC model evaluates, and the TrustZone tables of RM0456 Table 117.
 */
struct u5_rcc
{
  using source = u5::source;
  using signal = u5::signal;

  static constexpr bool                  msi               = true;
  static constexpr bool                  hsi_divider       = false;
  static constexpr std::optional<source> programming_clock = source::hsi16;

  static constexpr auto const& oscillators      = u5::oscillators;
  static constexpr auto const& names            = u5::names;
  static constexpr auto const& muxes            = u5::muxes;
  static constexpr auto const& signal_muxes     = u5::signal_muxes;
  static constexpr auto const& divided          = u5::divided;
  static constexpr auto const& internal_signals = u5::internal_signals;
  static constexpr auto const& rtc_sources      = u5::rtc_sources;
  static constexpr auto const& timers           = u5::timers;
  static constexpr auto const& enable_registers = u5::enable_registers;
  static constexpr auto const& pll_inputs       = u5::pll_inputs;
  static constexpr auto const& pll_sources      = u5::pll_sources;
  static constexpr auto const& system_inputs    = u5::system_inputs;
  static constexpr auto const& system_sources   = u5::system_sources;
  static constexpr auto const& pll_nodes        = u5::pll_nodes;
  static constexpr auto const& buses            = u5::buses;
  static constexpr auto const& reset_flags      = u5::reset_flags;
  static constexpr auto const& secured          = u5::rcc_secured;
  static constexpr auto const& unit_registers   = u5::rcc_unit_registers;
  static constexpr auto const& unit_aliases     = u5::rcc_unit_aliases;

  static constexpr std::string_view reset_flags_register = "RCC_CSR";
  static constexpr std::uint32_t    sw_mask              = 0b11;

  /**
   * @brief Timer kernel clock over its APB clock, RM0456 11.4.15: 1 when the APB prescaler
   *        divides by 1, else 2.
   * @param in Registers and names of the RCC.
   * @param prescaler Prescaler role of the timer's APB.
   */
  static auto timer_multiplier(rate_inputs const& in, rcc_role prescaler) -> std::uint64_t
  {
    return ppre_divider(in.read(prescaler)) == 1 ? 1 : 2;
  }

  /**
   * @brief Flags of `RCC_CSR` a reset of @p kind sets, RM0456 11.8.50.
   * @param kind Kind of the reset.
   */
  static constexpr auto reset_flags_of(reset_kind kind) noexcept -> std::array<std::string_view, 2>
  {
    switch(kind)
    {
      case reset_kind::power_on:
      case reset_kind::brown_out:
        return {"BORRSTF", "PINRSTF"};
      case reset_kind::pin:
        return {"PINRSTF", {}};
      case reset_kind::software:
        return {"SFTRSTF", {}};
      case reset_kind::watchdog:
        return {"IWDGRSTF", {}};
      case reset_kind::window_watchdog:
        return {"WWDGRSTF", {}};
      case reset_kind::low_power:
        return {"LPWRRSTF", {}};
      case reset_kind::option_load:
        return {"OBLRSTF", {}};
      case reset_kind::peripheral:
        return {};
    }
    return {};
  }
};
}
}
