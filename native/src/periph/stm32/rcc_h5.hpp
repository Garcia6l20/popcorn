#pragma once

#include "rcc_graph.hpp"

#include <popcorn/core/types.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace popcorn::periph::stm32
{
namespace
{
namespace h5
{
enum class source : std::uint8_t
{
  hsi,
  csi,
  hsi48,
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
  hsi,
  csi,
  hsi48,
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
  per,
  rtc,
};

using enum signal;

constexpr std::array<signal, 8> uart_inputs{pclk1, pll2_q, pll3_q, hsi, csi, lse};
constexpr std::array<signal, 8> lpuart_inputs{pclk3, pll2_q, pll3_q, hsi, csi, lse};
constexpr std::array<signal, 8> lptim_inputs{pclk3, pll2_p, pll3_r, lse, lsi, per};
constexpr std::array<signal, 8> audio_inputs{pll1_q, pll2_p, pll3_p, none, per};
constexpr std::array<signal, 8> spi_inputs_2{pclk2, pll2_q, pll3_q, hsi, csi, hse};
constexpr std::array<signal, 8> i2c_inputs_1{pclk1, pll3_r, hsi, csi};
constexpr std::array<signal, 8> i2c_inputs_3{pclk3, pll3_r, hsi, csi};

constexpr std::array<mux<signal>, 39> muxes{
    {
     {"RCC_CCIPR1",
         "USART1SEL",
         {pclk2, pll2_q, pll3_q, hsi, csi, lse},
         {{{"USART1", "USART1EN"}}}},
     {"RCC_CCIPR1", "USART2SEL", uart_inputs, {{{"USART2", "USART2EN"}}}},
     {"RCC_CCIPR1", "USART3SEL", uart_inputs, {{{"USART3", "USART3EN"}}}},
     {"RCC_CCIPR1", "UART4SEL", uart_inputs, {{{"UART4", "UART4EN"}}}},
     {"RCC_CCIPR1", "UART5SEL", uart_inputs, {{{"UART5", "UART5EN"}}}},
     {"RCC_CCIPR1", "USART6SEL", uart_inputs, {{{"USART6", "USART6EN"}}}},
     {"RCC_CCIPR1", "UART7SEL", uart_inputs, {{{"UART7", "UART7EN"}}}},
     {"RCC_CCIPR1", "UART8SEL", uart_inputs, {{{"UART8", "UART8EN"}}}},
     {"RCC_CCIPR1", "UART9SEL", uart_inputs, {{{"UART9", "UART9EN"}}}},
     {"RCC_CCIPR1", "USART10SEL", uart_inputs, {{{"USART10", "USART10EN"}}}},
     {"RCC_CCIPR2", "USART11SEL", uart_inputs, {{{"USART11", "USART11EN"}}}},
     {"RCC_CCIPR2", "USART12SEL", uart_inputs, {{{"UART12", "UART12EN"}}}},
     {"RCC_CCIPR2", "LPTIM1SEL", lptim_inputs, {{{"LPTIM1", "LPTIM1EN"}}}},
     {"RCC_CCIPR2",
         "LPTIM2SEL",
         {pclk1, pll2_p, pll3_r, lse, lsi, per},
         {{{"LPTIM2", "LPTIM2EN"}}}},
     {"RCC_CCIPR2", "LPTIM3SEL", lptim_inputs, {{{"LPTIM3", "LPTIM3EN"}}}},
     {"RCC_CCIPR2", "LPTIM4SEL", lptim_inputs, {{{"LPTIM4", "LPTIM4EN"}}}},
     {"RCC_CCIPR2", "LPTIM5SEL", lptim_inputs, {{{"LPTIM5", "LPTIM5EN"}}}},
     {"RCC_CCIPR2", "LPTIM6SEL", lptim_inputs, {{{"LPTIM6", "LPTIM6EN"}}}},
     {"RCC_CCIPR3", "SPI1SEL", audio_inputs, {{{"SPI1", "SPI1EN"}}}},
     {"RCC_CCIPR3", "SPI2SEL", audio_inputs, {{{"SPI2", "SPI2EN"}}}},
     {"RCC_CCIPR3", "SPI3SEL", audio_inputs, {{{"SPI3", "SPI3EN"}}}},
     {"RCC_CCIPR3", "SPI4SEL", spi_inputs_2, {{{"SPI4", "SPI4EN"}}}},
     {"RCC_CCIPR3", "SPI5SEL", {pclk3, pll2_q, pll3_q, hsi, csi, hse}, {{{"SPI5", "SPI5EN"}}}},
     {"RCC_CCIPR3", "SPI6SEL", spi_inputs_2, {{{"SPI6", "SPI6EN"}}}},
     {"RCC_CCIPR3", "LPUART1SEL", lpuart_inputs, {{{"LPUART", "LPUART1EN"}}}},
     {"RCC_CCIPR4", "OCTOSPI1SEL", {hclk, pll1_q, pll2_r, per}, {{{"OCTOSPI", "OCTOSPI1EN"}}}},
     {"RCC_CCIPR4", "USBFSSEL", {none, pll1_q, pll3_q, hsi48}, {{{"USB", "USBFSEN"}}}},
     {"RCC_CCIPR4", "SDMMC1SEL", {pll1_q, pll2_r}, {{{"SDMMC1", "SDMMC1EN"}}}},
     {"RCC_CCIPR4", "SDMMC2SEL", {pll1_q, pll2_r}, {{{"SDMMC2", "SDMMC2EN"}}}},
     {"RCC_CCIPR4", "I2C1SEL", i2c_inputs_1, {{{"I2C1", "I2C1EN"}}}},
     {"RCC_CCIPR4", "I2C2SEL", i2c_inputs_1, {{{"I2C2", "I2C2EN"}}}},
     {"RCC_CCIPR4", "I2C3SEL", i2c_inputs_3, {{{"I2C3", "I2C3EN"}}}},
     {"RCC_CCIPR4", "I2C4SEL", i2c_inputs_3, {{{"I2C4", "I2C4EN"}}}},
     {"RCC_CCIPR4", "I3C1SEL", {pclk1, pll3_r, hsi, none}, {{{"I3C", "I3C1EN"}}}},
     {"RCC_CCIPR5",
         "ADCDACSEL",
         {hclk, sysclk, pll2_r, hse, hsi, csi},
         {{{"ADC1", "ADC12EN"}, {"ADC2", "ADC12EN"}, {"ADCC", "ADC12EN"}, {"DAC", "DAC12EN"}}}},
     {"RCC_CCIPR5", "RNGSEL", {hsi48, pll1_q, lse, lsi}, {{{"RNG", "RNGEN"}}}},
     {"RCC_CCIPR5",
         "FDCAN12SEL",
         {hse, pll1_q, pll2_q, none},
         {{{"FDCAN1", "FDCAN12EN"}, {"FDCAN2", "FDCAN12EN"}}}},
     {"RCC_CCIPR5", "SAI1SEL", audio_inputs, {{{"SAI1", "SAI1EN"}}}},
     {"RCC_CCIPR5", "SAI2SEL", audio_inputs, {{{"SAI2", "SAI2EN"}}}},
     }
};

constexpr std::array<timer_names<signal>, 14> timers{
    {
     {"TIM2", "TIM2EN", pclk1},
     {"TIM3", "TIM3EN", pclk1},
     {"TIM4", "TIM4EN", pclk1},
     {"TIM5", "TIM5EN", pclk1},
     {"TIM6", "TIM6EN", pclk1},
     {"TIM7", "TIM7EN", pclk1},
     {"TIM12", "TIM12EN", pclk1},
     {"TIM13", "TIM13EN", pclk1},
     {"TIM14", "TIM14EN", pclk1},
     {"TIM1", "TIM1EN", pclk2},
     {"TIM8", "TIM8EN", pclk2},
     {"TIM15", "TIM15EN", pclk2},
     {"TIM16", "TIM16EN", pclk2},
     {"TIM17", "TIM17EN", pclk2},
     }
};

constexpr std::array<std::string_view, 7> enable_registers{
    "RCC_AHB1ENR",  "RCC_AHB2ENR", "RCC_AHB4ENR", "RCC_APB1LENR",
    "RCC_APB1HENR", "RCC_APB2ENR", "RCC_APB3ENR"};

constexpr std::uint64_t hsi_hz   = 64'000'000;
constexpr std::uint64_t csi_hz   = 4'000'000;
constexpr std::uint64_t hsi48_hz = 48'000'000;
constexpr std::uint64_t lsi_hz   = 32'000;

auto hsi_rate(rate_inputs const& in) -> std::uint64_t
{
  return hsi_hz >> in.read(rcc_role::hsi_divider);
}

auto rtc_prescaler(rate_inputs const& in) -> std::uint64_t
{
  auto const code = in.read(rcc_role::rtc_prescaler);
  return code < 2 ? 0 : code;
}

constexpr std::array<oscillator_names<signal>, 9> oscillators{
    {
     {"RCC_CR", "HSION", "HSIRDY", "HSIRDYF", "HSIRDYIE", hsi, &hsi_rate},
     {"RCC_CR", "CSION", "CSIRDY", "CSIRDYF", "CSIRDYIE", csi, &fixed_rate<csi_hz>},
     {"RCC_CR", "HSI48ON", "HSI48RDY", "HSI48RDYF", "HSI48RDYIE", hsi48, &fixed_rate<hsi48_hz>},
     {"RCC_CR", "HSEON", "HSERDY", "HSERDYF", "HSERDYIE", hse, &hse_rate},
     {"RCC_BDCR", "LSION", "LSIRDY", "LSIRDYF", "LSIRDYIE", lsi, &fixed_rate<lsi_hz>},
     {"RCC_BDCR", "LSEON", "LSERDY", "LSERDYF", "LSERDYIE", lse, &lse_rate},
     {"RCC_CR", "PLL1ON", "PLL1RDY", "PLL1RDYF", "PLL1RDYIE"},
     {"RCC_CR", "PLL2ON", "PLL2RDY", "PLL2RDYF", "PLL2RDYIE"},
     {"RCC_CR", "PLL3ON", "PLL3RDY", "PLL3RDYF", "PLL3RDYIE"},
     }
};

constexpr auto names = std::to_array<named_field<rcc_role>>({
    {rcc_role::css_on,             {"RCC_CR", "HSECSSON"} },
    {rcc_role::msi_select,         {}                     },
    {rcc_role::msis_range,         {}                     },
    {rcc_role::msik_range,         {}                     },
    {rcc_role::msis_standby_range, {}                     },
    {rcc_role::msik_standby_range, {}                     },
    {rcc_role::sw,                 {"RCC_CFGR1", "SW"}    },
    {rcc_role::sws,                {"RCC_CFGR1", "SWS"}   },
    {rcc_role::hpre,               {"RCC_CFGR2", "HPRE"}  },
    {rcc_role::ppre1,              {"RCC_CFGR2", "PPRE1"} },
    {rcc_role::ppre2,              {"RCC_CFGR2", "PPRE2"} },
    {rcc_role::ppre3,              {"RCC_CFGR2", "PPRE3"} },
    {rcc_role::rtc_select,         {"RCC_BDCR", "RTCSEL"} },
    {rcc_role::rtc_enable,         {"RCC_BDCR", "RTCEN"}  },
    {rcc_role::lsi_prediv,         {}                     },
    {rcc_role::backup_reset,       {"RCC_BDCR", "VSWRST"} },
    {rcc_role::remove_flags,       {"RCC_RSR", "RMVF"}    },
    {rcc_role::hsi_divider,        {"RCC_CR", "HSIDIV"}   },
    {rcc_role::hsi_divider_flag,   {"RCC_CR", "HSIDIVF"}  },
    {rcc_role::timer_prescaler,    {"RCC_CFGR1", "TIMPRE"}},
    {rcc_role::rtc_prescaler,      {"RCC_CFGR1", "RTCPRE"}},
});

static_assert(names_every_role(names));

constexpr std::array<signal_mux<signal>, 1> signal_muxes{
    {
     {"RCC_CCIPR5", "CKPERSEL", {hsi, csi, hse, none}, per},
     }
};

constexpr std::array<divided_signal<signal>, 0> divided{};

constexpr std::array<signal, 0> internal_signals{};

constexpr std::array<rtc_input<signal>, 4> rtc_sources{
    {{none, &divide_by<1>}, {lse, &divide_by<1>}, {lsi, &divide_by<1>}, {hse, &rtc_prescaler}}
};

constexpr std::array<source, 4> pll_inputs{source::pll1, source::hsi, source::csi, source::hse};
constexpr std::array<signal, 4> pll_sources{none, hsi, csi, hse};
constexpr std::array<std::optional<source>, 4> system_inputs{
    source::hsi, source::csi, source::hse, source::pll1};
constexpr std::array<signal, 4> system_sources{hsi, csi, hse, pll1_p};

constexpr std::array<bus_node<signal>, 3> buses{
    {{pclk1, rcc_role::ppre1}, {pclk2, rcc_role::ppre2}, {pclk3, rcc_role::ppre3}}
};

constexpr std::array<pll_node<signal>, 3> pll_nodes{
    {
     {.config                = "RCC_PLL1CFGR",
         .dividers              = "RCC_PLL1DIVR",
         .fraction              = "RCC_PLL1FRACR",
         .src                   = "PLL1SRC",
         .range                 = "PLL1RGE",
         .m                     = "DIVM1",
         .fracen                = "PLL1FRACEN",
         .n                     = "PLL1N",
         .frac                  = "PLL1FRACN",
         .divider               = {"PLL1P", "PLL1Q", "PLL1R"},
         .output_enable         = {"PLL1PEN", "PLL1QEN", "PLL1REN"},
         .out                   = {pll1_p, pll1_q, pll1_r},
         .outputs               = 3,
         .m_decode              = &direct,
         .n_decode              = &plus_one,
         .output_decode         = {&plus_one, &plus_one, &plus_one},
         .outputs_held_while_on = true},
     {.config        = "RCC_PLL2CFGR",
         .dividers      = "RCC_PLL2DIVR",
         .fraction      = "RCC_PLL2FRACR",
         .src           = "PLL2SRC",
         .range         = "PLL2RGE",
         .m             = "DIVM2",
         .fracen        = "PLL2FRACEN",
         .n             = "PLL2N",
         .frac          = "PLL2FRACN",
         .divider       = {"PLL2P", "PLL2Q", "PLL2R"},
         .output_enable = {"PLL2PEN", "PLL2QEN", "PLL2REN"},
         .out           = {pll2_p, pll2_q, pll2_r},
         .outputs       = 3,
         .m_decode      = &direct,
         .n_decode      = &plus_one,
         .output_decode = {&plus_one, &plus_one, &plus_one}},
     {.config        = "RCC_PLL3CFGR",
         .dividers      = "RCC_PLL3DIVR",
         .fraction      = "RCC_PLL3FRACR",
         .src           = "PLL3SRC",
         .range         = "PLL3RGE",
         .m             = "DIVM3",
         .fracen        = "PLL3FRACEN",
         .n             = "PLL3N",
         .frac          = "PLL3FRACN",
         .divider       = {"PLL3P", "PLL3Q", "PLL3R"},
         .output_enable = {"PLL3PEN", "PLL3QEN", "PLL3REN"},
         .out           = {pll3_p, pll3_q, pll3_r},
         .outputs       = 3,
         .m_decode      = &direct,
         .n_decode      = &plus_one,
         .output_decode = {&plus_one, &plus_one, &plus_one}},
     }
};

constexpr std::array<std::string_view, 6> reset_flags{"BORRSTF",  "PINRSTF",  "SFTRSTF",
                                                      "IWDGRSTF", "WWDGRSTF", "LPWRRSTF"};

constexpr auto rcc_secured = std::to_array<secured_group>({
    {"HSISEC",      "RCC_CR",        "HSION HSIKERON HSIRDY"                                            },
    {"HSISEC",      "RCC_HSICFGR",   "HSICAL HSITRIM"                                                   },
    {"HSISEC",      "RCC_CIER",      "HSIRDYIE"                                                         },
    {"HSISEC",      "RCC_CIFR",      "HSIRDYF"                                                          },
    {"HSISEC",      "RCC_CICR",      "HSIRDYC"                                                          },
    {"HSESEC",      "RCC_CR",        "HSEON HSERDY HSEBYP HSECSSON HSEEXT"                              },
    {"HSESEC",      "RCC_CIER",      "HSERDYIE"                                                         },
    {"HSESEC",      "RCC_CIFR",      "HSERDYF HSECSSF"                                                  },
    {"HSESEC",      "RCC_CICR",      "HSERDYC HSECSSC"                                                  },
    {"CSISEC",      "RCC_CR",        "CSION CSIKERON CSIRDY"                                            },
    {"CSISEC",      "RCC_CSICFGR",   "CSICAL CSITRIM"                                                   },
    {"CSISEC",      "RCC_CIER",      "CSIRDYIE"                                                         },
    {"CSISEC",      "RCC_CIFR",      "CSIRDYF"                                                          },
    {"CSISEC",      "RCC_CICR",      "CSIRDYC"                                                          },
    {"LSISEC",      "RCC_BDCR",      "LSION LSIRDY LSCOSEL LSCOEN"                                      },
    {"LSISEC",      "RCC_CIER",      "LSIRDYIE"                                                         },
    {"LSISEC",      "RCC_CIFR",      "LSIRDYF"                                                          },
    {"LSISEC",      "RCC_CICR",      "LSIRDYC"                                                          },
    {"LSESEC",      "RCC_BDCR",      "LSECSSON LSECSSD LSEDRV LSEBYP LSERDY LSEON LSEEXT LSCOSEL LSCOEN"},
    {"LSESEC",      "RCC_CIER",      "LSERDYIE"                                                         },
    {"LSESEC",      "RCC_CIFR",      "LSERDYF"                                                          },
    {"LSESEC",      "RCC_CICR",      "LSERDYC"                                                          },
    {"SYSCLKSEC",   "RCC_CFGR1",     "SW SWS STOPWUCK STOPKERWUCK MCO1SEL MCO1PRE MCO2SEL MCO2PRE"      },
    {"SYSCLKSEC",   "RCC_CCIPR4",    "SYSTICKSEL"                                                       },
    {"PRESCSEC",    "RCC_CFGR1",     "TIMPRE"                                                           },
    {"PRESCSEC",    "RCC_CFGR2",     "HPRE PPRE1 PPRE2 PPRE3"                                           },
    {"PLL1SEC",     "RCC_PLL1CFGR",
     "PLL1SRC PLL1RGE PLL1FRACEN DIVM1 PLL1VCOSEL PLL1PEN PLL1QEN PLL1REN"                              },
    {"PLL1SEC",     "RCC_PLL1DIVR",  "PLL1N PLL1P PLL1Q PLL1R"                                          },
    {"PLL1SEC",     "RCC_PLL1FRACR", "PLL1FRACN"                                                        },
    {"PLL1SEC",     "RCC_CR",        "PLL1RDY PLL1ON"                                                   },
    {"PLL1SEC",     "RCC_CIER",      "PLL1RDYIE"                                                        },
    {"PLL1SEC",     "RCC_CIFR",      "PLL1RDYF"                                                         },
    {"PLL1SEC",     "RCC_CICR",      "PLL1RDYC"                                                         },
    {"PLL2SEC",     "RCC_PLL2CFGR",
     "PLL2SRC PLL2RGE PLL2FRACEN DIVM2 PLL2VCOSEL PLL2PEN PLL2QEN PLL2REN"                              },
    {"PLL2SEC",     "RCC_PLL2DIVR",  "PLL2N PLL2P PLL2Q PLL2R"                                          },
    {"PLL2SEC",     "RCC_PLL2FRACR", "PLL2FRACN"                                                        },
    {"PLL2SEC",     "RCC_CR",        "PLL2RDY PLL2ON"                                                   },
    {"PLL2SEC",     "RCC_CIER",      "PLL2RDYIE"                                                        },
    {"PLL2SEC",     "RCC_CIFR",      "PLL2RDYF"                                                         },
    {"PLL2SEC",     "RCC_CICR",      "PLL2RDYC"                                                         },
    {"PLL3SEC",     "RCC_PLL3CFGR",
     "PLL3SRC PLL3RGE PLL3FRACEN DIVM3 PLL3VCOSEL PLL3PEN PLL3QEN PLL3REN"                              },
    {"PLL3SEC",     "RCC_PLL3DIVR",  "PLL3N PLL3P PLL3Q PLL3R"                                          },
    {"PLL3SEC",     "RCC_PLL3FRACR", "PLL3FRACN"                                                        },
    {"PLL3SEC",     "RCC_CR",        "PLL3RDY PLL3ON"                                                   },
    {"PLL3SEC",     "RCC_CIER",      "PLL3RDYIE"                                                        },
    {"PLL3SEC",     "RCC_CIFR",      "PLL3RDYF"                                                         },
    {"PLL3SEC",     "RCC_CICR",      "PLL3RDYC"                                                         },
    {"HSI48SEC",    "RCC_CR",        "HSI48ON HSI48RDY"                                                 },
    {"HSI48SEC",    "RCC_CRRCR",     "HSI48CAL"                                                         },
    {"HSI48SEC",    "RCC_CIER",      "HSI48RDYIE"                                                       },
    {"HSI48SEC",    "RCC_CIFR",      "HSI48RDYF"                                                        },
    {"HSI48SEC",    "RCC_CICR",      "HSI48RDYC"                                                        },
    {"CKPERSELSEC", "RCC_CCIPR5",    "CKPERSEL"                                                         },
    {"RMVFSEC",     "RCC_RSR",       "RMVF"                                                             },
});

constexpr auto rcc_unit_registers = std::to_array<std::string_view>({
    "RCC_AHB1RSTR",  "RCC_AHB2RSTR",  "RCC_AHB4RSTR",   "RCC_APB1LRSTR",  "RCC_APB1HRSTR",
    "RCC_APB2RSTR",  "RCC_APB3RSTR",  "RCC_AHB1ENR",    "RCC_AHB2ENR",    "RCC_AHB4ENR",
    "RCC_APB1LENR",  "RCC_APB1HENR",  "RCC_APB2ENR",    "RCC_APB3ENR",    "RCC_AHB1LPENR",
    "RCC_AHB2LPENR", "RCC_AHB4LPENR", "RCC_APB1LLPENR", "RCC_APB1HLPENR", "RCC_APB2LPENR",
    "RCC_APB3LPENR", "RCC_CCIPR1",    "RCC_CCIPR2",     "RCC_CCIPR3",     "RCC_CCIPR4",
    "RCC_CCIPR5",
});

constexpr auto rcc_unit_suffixes = std::to_array<std::string_view>({"LPEN", "RST", "EN", "SEL"});

constexpr auto rcc_backup_secured = std::to_array<secured_group>({
    {"", "RCC_CFGR1", "RTCPRE"      },
    {"", "RCC_BDCR",  "RTCEN RTCSEL"},
});

constexpr auto rcc_unit_aliases = std::to_array<unit_alias>({
    {"ADC12",     "ADC1"   },
    {"ADCDAC",    "ADC1"   },
    {"ADCDAC",    "DAC"    },
    {"DAC12",     "DAC"    },
    {"DCMI_PSSI", "DCMI"   },
    {"ETHTX",     "ETH"    },
    {"ETHRX",     "ETH"    },
    {"FDCAN12",   "FDCAN1" },
    {"FDCAN12",   "FDCAN2" },
    {"FLITF",     "FLASH"  },
    {"I3C1",      "I3C"    },
    {"LPUART1",   "LPUART" },
    {"OCTOSPI1",  "OCTOSPI"},
    {"RTCAPB",    "RTC"    },
    {"TIMIC",     "TIM12"  },
    {"TIMIC",     "TIM15"  },
    {"TIMIC",     "LPTIM2" },
    {"UCPD",      "UCPD1"  },
    {"USART12",   "UART12" },
    {"USBFS",     "USB"    },
    {"VREF",      "VREFBUF"},
});
}

/**
 * @brief Clock tree of the STM32H5 RCC, RM0481 section 11: HSI over HSIDIV, CSI, HSI48, HSE,
 *        LSI, LSE and PLL1 to PLL3, SYSCLK from the PLL1 P output, the kernel clock muxes of
 *        RCC_CCIPR1 to RCC_CCIPR5 with per_ck, and TIMPRE. The TrustZone tables are RM0481
 *        11.5.1 and Table 122 in the SVD field names: `HSIRDYF`, `CSION`, `CSIRDY`, `DIVMx`,
 *        `HSI48RDYIE`, `CKPERSELSEC` for `IPKERSECCFG`, and `RMVF` in `RCC_RSR`. `TIMPRE` goes
 *        with PRESCSEC. A Secure RTC or TAMP secures `RTCPRE`, `RTCEN` and `RTCSEL`.
 *        `ADCDACSEL` is Secure while ADC1, ADC2 or the DAC is, the `FDCAN12` bits while FDCAN1
 *        or FDCAN2 is, and `FLITFEN` and `FLITFLPEN` follow the FLASH as `FLASHEN` does on the
 *        STM32U5.
 */
struct h5_rcc
{
  using source = h5::source;
  using signal = h5::signal;

  static constexpr bool msi         = false;
  static constexpr bool hsi_divider = true;
  /**
   * @brief Time from an `HSIDIV` change to `HSIDIVF` set. An STM32H573 sets the flag within 35
   *        cycles at 32 MHz, the longest transition being /2 to /1.
   */
  static constexpr std::chrono::nanoseconds hsi_divider_delay = std::chrono::microseconds{1};
  /**
   * @brief RM0481 11.8.1: HSEON, HSION and CSION hold while their oscillator feeds PLL1 with
   *        PLL1ON set.
   */
  static constexpr bool                  pll1_input_held_while_on = true;
  static constexpr source                divided_oscillator       = source::hsi;
  static constexpr std::optional<source> programming_clock        = std::nullopt;

  static constexpr auto const& oscillators      = h5::oscillators;
  static constexpr auto const& names            = h5::names;
  static constexpr auto const& muxes            = h5::muxes;
  static constexpr auto const& signal_muxes     = h5::signal_muxes;
  static constexpr auto const& divided          = h5::divided;
  static constexpr auto const& internal_signals = h5::internal_signals;
  static constexpr auto const& rtc_sources      = h5::rtc_sources;
  static constexpr auto const& timers           = h5::timers;
  static constexpr auto const& enable_registers = h5::enable_registers;
  static constexpr auto const& pll_inputs       = h5::pll_inputs;
  static constexpr auto const& pll_sources      = h5::pll_sources;
  static constexpr auto const& system_inputs    = h5::system_inputs;
  static constexpr auto const& system_sources   = h5::system_sources;
  static constexpr auto const& pll_nodes        = h5::pll_nodes;
  static constexpr auto const& buses            = h5::buses;
  static constexpr auto const& reset_flags      = h5::reset_flags;
  static constexpr auto const& secured          = h5::rcc_secured;
  static constexpr auto const& unit_registers   = h5::rcc_unit_registers;
  static constexpr auto const& unit_aliases     = h5::rcc_unit_aliases;
  static constexpr auto const& unit_suffixes    = h5::rcc_unit_suffixes;
  static constexpr auto const& backup_secured   = h5::rcc_backup_secured;

  static constexpr std::string_view reset_flags_register = "RCC_RSR";
  static constexpr std::uint32_t    sw_mask              = 0b11;

  /**
   * @brief Timer kernel clock over its APB clock, RM0481 11.8.5 TIMPRE: with TIMPRE 0, 1 when
   *        the APB prescaler divides by 1, else 2. With TIMPRE 1, 2 up to a division by 4,
   *        else 4.
   * @param in Registers and names of the RCC.
   * @param prescaler Prescaler role of the timer's APB.
   */
  static auto timer_multiplier(rate_inputs const& in, rcc_role prescaler) -> std::uint64_t
  {
    auto const divider = ppre_divider(in.read(prescaler));
    if(in.read(rcc_role::timer_prescaler) == 0)
    {
      return divider == 1 ? 1 : 2;
    }
    return divider <= 4 ? 2 : 4;
  }

  /**
   * @brief Flags of `RCC_RSR` a reset of @p kind sets, RM0481 11.8.46: BORRSTF, PINRSTF,
   *        SFTRSTF, IWDGRSTF, WWDGRSTF and LPWRRSTF.
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
      case reset_kind::peripheral:
        return {};
    }
    return {};
  }
};
}
}
