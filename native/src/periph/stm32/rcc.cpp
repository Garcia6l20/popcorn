#include "backup_domain.hpp"
#include "factories.hpp"
#include "gtzc.hpp"
#include "hsi16_source.hpp"
#include "kernel_gate.hpp"
#include "lsi_source.hpp"
#include "model_base.hpp"
#include "rcc_resets.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
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

constexpr std::size_t   source_count = 11;
constexpr std::uint32_t switch_kind  = source_count;

struct oscillator_names
{
  std::string_view reg;
  std::string_view on;
  std::string_view ready;
  std::string_view flag;
  std::string_view enable;
};

constexpr std::array<oscillator_names, source_count> oscillators{
    {
     {"RCC_CR", "MSISON", "MSISRDY", "MSISRDYF", "MSISRDYIE"},
     {"RCC_CR", "MSIKON", "MSIKRDY", "MSIKRDYF", "MSIKRDYIE"},
     {"RCC_CR", "HSION", "HSIRDY", "HSIRDYF", "HSIRDYIE"},
     {"RCC_CR", "HSI48ON", "HSI48RDY", "HSI48RDYF", "HSI48RDYIE"},
     {"RCC_CR", "SHSION", "SHSIRDY", "SHSIRDYF", "SHSIRDYIE"},
     {"RCC_CR", "HSEON", "HSERDY", "HSERDYF", "HSERDYIE"},
     {"RCC_BDCR", "LSION", "LSIRDY", "LSIRDYF", "LSIRDYIE"},
     {"RCC_BDCR", "LSEON", "LSERDY", "LSERDYF", "LSERDYIE"},
     {"RCC_CR", "PLL1ON", "PLL1RDY", "PLL1RDYF", "PLL1RDYIE"},
     {"RCC_CR", "PLL2ON", "PLL2RDY", "PLL2RDYF", "PLL2RDYIE"},
     {"RCC_CR", "PLL3ON", "PLL3RDY", "PLL3RDYF", "PLL3RDYIE"},
     }
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

constexpr std::size_t signal_count = 25;

using enum signal;

struct consumer
{
  std::string_view instance;
  std::string_view enable;
};

struct mux
{
  std::string_view        reg;
  std::string_view        field;
  std::array<signal, 8>   inputs;
  std::array<consumer, 3> consumers;
};

constexpr std::array<signal, 8> uart_inputs_1{pclk1, sysclk, hsi16, lse};
constexpr std::array<signal, 8> i2c_inputs_1{pclk1, sysclk, hsi16, msik};
constexpr std::array<signal, 8> low_power_inputs{msik, lsi, hsi16, lse};
constexpr std::array<signal, 8> audio_inputs{hclk, pll1_p, pll3_q, none, msik};
constexpr std::array<signal, 8> sai_inputs{pll2_p, pll3_p, pll1_p, none, hsi16};
constexpr std::array<signal, 8> pclk3_inputs{pclk3, sysclk, hsi16, msik};

constexpr std::array<mux, 28> muxes{
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
         {{{"ADC1", "ADC12EN"}, {"ADC4", "ADC4EN"}, {"DAC1", "DAC1EN"}}}},
     {"RCC_CCIPR3", "ADF1SEL", audio_inputs, {{{"ADF1", "ADF1EN"}}}},
     }
};

struct timer_names
{
  std::string_view instance;
  std::string_view enable;
  signal           bus;
};

constexpr std::array<timer_names, 11> timers{
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
constexpr std::uint64_t frac_one = 8192;

constexpr auto hpre_divider(std::uint32_t code) noexcept -> std::uint64_t
{
  constexpr std::array<std::uint64_t, 8> high{2, 4, 8, 16, 64, 128, 256, 512};
  return code < 8 ? 1 : high[code - 8];
}

constexpr auto ppre_divider(std::uint32_t code) noexcept -> std::uint64_t
{
  return code < 4 ? 1 : std::uint64_t{2} << (code - 4);
}

constexpr auto flags_of(reset_kind kind) noexcept -> std::array<std::string_view, 2>
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
  }
  return {};
}

constexpr std::array<std::string_view, 7> reset_flags{"BORRSTF",  "PINRSTF",  "SFTRSTF", "IWDGRSTF",
                                                      "WWDGRSTF", "LPWRRSTF", "OBLRSTF"};

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

struct secured_group
{
  std::string_view bit;
  std::string_view reg;
  std::string_view fields;
};

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

constexpr auto rcc_unit_suffixes =
    std::to_array<std::string_view>({"SMEN", "AMEN", "RST", "EN", "SEL"});

using unit_alias = std::pair<std::string_view, std::string_view>;

constexpr auto rcc_unit_aliases = std::to_array<unit_alias>({
    {"DCMI_PSSI", "DCMI"   },
    {"FSMC",      "FMC"    },
    {"OTG",       "OTG_FS" },
    {"ICACHE",    "ICache" },
    {"DCACHE1",   "DCACHE" },
    {"VREF",      "VREFBUF"},
    {"RTCAPB",    "RTC"    },
});

struct unit_bits
{
  std::uint32_t          offset{};
  std::uint32_t          bits{};
  std::size_t            unit{};
  trustzone_aware const* aware{};
};

struct rcc_state
{
  std::array<std::uint32_t, source_count + 1> generation{};
  std::uint32_t                               pending{};
  std::uint8_t                                lsi_forced{};
  std::uint8_t                                hsi16_held{};

  friend auto operator==(rcc_state const&, rcc_state const&) -> bool = default;
};

struct oscillator
{
  field_ref on;
  field_ref ready;
  field_ref flag;
  field_ref enable;
};

struct pll_fields
{
  field_ref                src;
  field_ref                range;
  field_ref                m;
  field_ref                fracen;
  field_ref                n;
  field_ref                frac;
  std::array<field_ref, 3> divider;
  std::array<field_ref, 3> output_enable;
};

struct kernel
{
  core::clock_domain           domain;
  field_ref                    select;
  std::array<signal, 8> const* inputs;
  field_ref                    enable;
};

struct reset_entry
{
  std::string instance;
  std::size_t reg;
  field_ref   bit;
};

struct timer
{
  core::clock_domain domain;
  signal             bus;
  field_ref          prescaler;
  field_ref          enable;
};

/**
 * @brief RCC of the STM32U5, RM0456 section 11. Ready flags follow their enables after the
 *        board's ready delay. RCC writes RCC_CIFR storage directly when the matching RCC_CIER bit
 *        is set and raises no ready interrupt: firmware waiting on the RCC IRQ hangs, and the
 *        spin stop names its loop. A write that would stop the core clock, switching off the
 *        oscillator that drives it or changing PLL1 while it drives it, is refused and logs one
 *        warning per machine life. A started IWDG keeps the LSI on until the next reset, RM0456
 *        section 11.4.16. FLASH programming or erase keeps HSI16 on while it runs, RM0456
 *        sections 7.3.6 and 7.3.7. A 0 to 1 write of an instance's bit in an RCC_xxxRSTR
 *        register, RM0456 11.8.21 to 11.8.28, calls `bus_reset_listener::on_bus_reset()` on
 *        the instance's model when it listens. The bit is `<instance>RST`, or the name of
 *        `reset_bit_names`. `kernel_gate::held_in_reset()` reads the bit. A bit of an instance
 *        with no listening model is storage.
 *
 *        TrustZone, RM0456 11.5 and Table 117: the bits each `RCC_SECCFGR` bit secures, and the
 *        reset, enable, sleep enable, autonomous enable and kernel clock selection bits of a
 *        Secure peripheral, are RAZ/WI to a Non-secure access, with no event. A peripheral is
 *        Secure by its TZSC bit, or for a TrustZone-aware model by its own security bits, the
 *        FLASH by RM0456 Table 62. A bit whose name gives no peripheral of the part stays
 *        Non-secure, such as the GTZCn and shared kernel clock selections. Deviation: SRAMnEN
 *        and SRAMnSMEN stay Non-secure, the MPCBB and INVSECSTATE rule of RM0456 5.4 is not
 *        modelled. `BDRST` is
 *        Secure while the RTC or the TAMP has a secure function, RM0456 11.5.1. `SPRIV` makes
 *        the Secure bits privileged, `NSPRIV` the others, RAZ/WI to an unprivileged access.
 *        `RCC_SECCFGR` reads for every transaction and takes Secure writes, privileged with SPRIV
 *        set. A Non-secure write to it is ignored with an illegal access event. `RCC_PRIVCFGR`
 *        takes privileged writes, SPRIV Secure ones only. A write of `RCC_CICR` clears the
 *        `RCC_CIFR` flags of the description's cross rules, the flags of a Secure oscillator only
 *        for a Secure write. Under TZEN=0 `RCC_SECCFGR` is RAZ/WI, RM0456 11.8.51, and
 *        `RCC_PRIVCFGR` is storage.
 */
class rcc final : public dispatch_model<rcc, rcc_state>,
                  public lsi_source,
                  public hsi16_source,
                  public kernel_gate,
                  public trustzone_aware
{
public:
  rcc(model_context& ctx, peripheral_desc const& desc, rcc_options const& options);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto drives_core_clock() const noexcept -> bool override
  {
    return true;
  }
  void               force_lsi() override;
  void               hold_hsi16(bool held) override;
  [[nodiscard]] auto bus_enabled(core::clock_domain kernel) const -> bool override;
  [[nodiscard]] auto reset_line(std::string_view instance) const
      -> std::optional<std::size_t> override;
  [[nodiscard]] auto held_in_reset(std::size_t line) const -> bool override;

  void                            cr_written(std::uint32_t old, std::uint32_t now);
  void                            icscr1_written(std::uint32_t old, std::uint32_t now);
  void                            tree_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Pll> void pll_config_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Pll> void pll_dividers_written(std::uint32_t old, std::uint32_t now);
  void                            bdcr_written(std::uint32_t old, std::uint32_t now);
  void                            csr_written(std::uint32_t old, std::uint32_t now);
  void                            cicr_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Reg> void resets_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto              secure_function() const -> bool override;
  [[nodiscard]] auto              secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void               add_security_rules();
  void               resolve_units();
  [[nodiscard]] auto unit_is_secure(unit_bits const& rule) const -> bool;

  [[nodiscard]] auto is_ready(source which) const -> bool;
  [[nodiscard]] auto is_on(source which) const -> bool;
  [[nodiscard]] auto pll_input(std::size_t pll) const -> source;
  [[nodiscard]] auto system_source(std::uint32_t code) const -> source;
  [[nodiscard]] auto in_use() const -> std::uint32_t;
  [[nodiscard]] auto locked(std::size_t pll) const -> bool;
  [[nodiscard]] auto rates() const -> std::array<std::uint64_t, signal_count>;
  [[nodiscard]] auto enable_field(std::string_view name) const -> field_ref;

  void oscillators_written(
      std::string_view reg,
      std::uint32_t    word,
      std::uint32_t    old,
      std::uint32_t    now);
  void start(source which);
  void stop(source which);
  void schedule(std::uint32_t kind);
  void cancel(std::uint32_t kind);
  void settle(bool notify);
  void publish(bool notify);
  void load(std::span<std::byte const> in) override;
  void refuse_core_stop(std::string_view reg, std::string_view what);

  bool                                                 core_stop_logged_{};
  rcc_options                                          options_;
  register_file&                                       regs_;
  std::array<oscillator, source_count>                 osc_{};
  std::array<pll_fields, 3>                            pll_{};
  field_ref                                            css_on_;
  field_ref                                            msi_select_;
  field_ref                                            msis_range_;
  field_ref                                            msik_range_;
  field_ref                                            msis_standby_range_;
  field_ref                                            msik_standby_range_;
  field_ref                                            sw_;
  field_ref                                            sws_;
  field_ref                                            hpre_;
  std::array<field_ref, 3>                             ppre_{};
  field_ref                                            iclk_select_;
  field_ref                                            rtc_select_;
  field_ref                                            rtc_enable_;
  field_ref                                            lsi_prediv_;
  field_ref                                            backup_reset_;
  field_ref                                            remove_flags_;
  std::array<field_ref, reset_flags.size()>            flags_{};
  reg_ref                                              bdcr_;
  reg_ref                                              csr_;
  std::vector<std::pair<core::clock_domain, signal>>   signals_;
  std::vector<kernel>                                  kernels_;
  std::vector<reset_entry>                             resets_;
  std::vector<std::uint8_t>                            kernel_enables_;
  std::vector<timer>                                   timers_;
  std::optional<reg_ref>                               secure_;
  reg_ref                                              privilege_;
  field_ref                                            secure_privileged_;
  field_ref                                            nonsecure_privileged_;
  std::vector<std::pair<field_ref, field_ref>>         secured_;
  std::vector<unit_bits>                               units_;
  std::vector<cross_ref>                               flag_clears_;
  std::array<trustzone_aware const*, 2>                backup_units_{};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> words_;
  peripheral_security*                                 firewall_{};
  bool                                                 units_resolved_{};
  std::optional<std::size_t>                           unit_;
  illegal_event                                        illegal_;
};

constexpr std::array<write_entry<rcc>, 27> rcc_writes{
    {
     {"RCC_CR", &rcc::cr_written},
     {"RCC_ICSCR1", &rcc::icscr1_written},
     {"RCC_CFGR1", &rcc::tree_written},
     {"RCC_CFGR2", &rcc::tree_written},
     {"RCC_CFGR3", &rcc::tree_written},
     {"RCC_PLL1CFGR", &rcc::pll_config_written<0>},
     {"RCC_PLL2CFGR", &rcc::pll_config_written<1>},
     {"RCC_PLL3CFGR", &rcc::pll_config_written<2>},
     {"RCC_PLL1DIVR", &rcc::pll_dividers_written<0>},
     {"RCC_PLL2DIVR", &rcc::pll_dividers_written<1>},
     {"RCC_PLL3DIVR", &rcc::pll_dividers_written<2>},
     {"RCC_PLL1FRACR", &rcc::tree_written},
     {"RCC_PLL2FRACR", &rcc::tree_written},
     {"RCC_PLL3FRACR", &rcc::tree_written},
     {"RCC_CCIPR1", &rcc::tree_written},
     {"RCC_CCIPR2", &rcc::tree_written},
     {"RCC_CCIPR3", &rcc::tree_written},
     {"RCC_AHB1ENR", &rcc::tree_written},
     {"RCC_AHB2ENR1", &rcc::tree_written},
     {"RCC_AHB2ENR2", &rcc::tree_written},
     {"RCC_AHB3ENR", &rcc::tree_written},
     {"RCC_APB1ENR1", &rcc::tree_written},
     {"RCC_APB1ENR2", &rcc::tree_written},
     {"RCC_APB2ENR", &rcc::tree_written},
     {"RCC_APB3ENR", &rcc::tree_written},
     {"RCC_BDCR", &rcc::bdcr_written},
     {"RCC_CSR", &rcc::csr_written},
     }
};

constexpr std::array<write_entry<rcc>, 8> rcc_reset_writes{
    {
     {reset_registers[0], &rcc::resets_written<0>},
     {reset_registers[1], &rcc::resets_written<1>},
     {reset_registers[2], &rcc::resets_written<2>},
     {reset_registers[3], &rcc::resets_written<3>},
     {reset_registers[4], &rcc::resets_written<4>},
     {reset_registers[5], &rcc::resets_written<5>},
     {reset_registers[6], &rcc::resets_written<6>},
     {reset_registers[7], &rcc::resets_written<7>},
     }
};

constexpr std::array<write_entry<rcc>, 1> rcc_secure_writes{
    {
     {"RCC_SECCFGR", nullptr},
     }
};

constexpr auto rcc_trustzone_writes = std::to_array<write_entry<rcc>>({
    {"RCC_ICSCR2",     nullptr           },
    {"RCC_ICSCR3",     nullptr           },
    {"RCC_CRRCR",      nullptr           },
    {"RCC_CIER",       nullptr           },
    {"RCC_CIFR",       nullptr           },
    {"RCC_CICR",       &rcc::cicr_written},
    {"RCC_AHB1SMENR",  nullptr           },
    {"RCC_AHB2SMENR1", nullptr           },
    {"RCC_AHB2SMENR2", nullptr           },
    {"RCC_AHB3SMENR",  nullptr           },
    {"RCC_APB1SMENR1", nullptr           },
    {"RCC_APB1SMENR2", nullptr           },
    {"RCC_APB2SMENR",  nullptr           },
    {"RCC_APB3SMENR",  nullptr           },
    {"RCC_SRDAMR",     nullptr           },
    {"RCC_PRIVCFGR",   nullptr           },
});

rcc::rcc(model_context& ctx, peripheral_desc const& desc, rcc_options const& options)
    : dispatch_model{ctx, desc, "RCC", rcc_writes, rcc_trustzone_writes}, options_{options},
      regs_{ctx.regs}, css_on_{field("RCC_CR", "CSSON")},
      msi_select_{field("RCC_ICSCR1", "MSIRGSEL")}, msis_range_{field("RCC_ICSCR1", "MSISRANGE")},
      msik_range_{field("RCC_ICSCR1", "MSIKRANGE")},
      msis_standby_range_{field("RCC_CSR", "MSISSRANGE")},
      msik_standby_range_{field("RCC_CSR", "MSIKSRANGE")}, sw_{field("RCC_CFGR1", "SW")},
      sws_{field("RCC_CFGR1", "SWS")}, hpre_{field("RCC_CFGR2", "HPRE")},
      ppre_{field("RCC_CFGR2", "PPRE1"), field("RCC_CFGR2", "PPRE2"), field("RCC_CFGR3", "PPRE3")},
      iclk_select_{field("RCC_CCIPR1", "ICLKSEL")}, rtc_select_{field("RCC_BDCR", "RTCSEL")},
      rtc_enable_{field("RCC_BDCR", "RTCEN")}, lsi_prediv_{field("RCC_BDCR", "LSIPREDIV")},
      backup_reset_{field("RCC_BDCR", "BDRST")}, remove_flags_{field("RCC_CSR", "RMVF")},
      bdcr_{reg("RCC_BDCR")}, csr_{reg("RCC_CSR")}, unit_{unit_named(ctx.part, desc.name)}
{
  for(auto const& entry : rcc_reset_writes)
  {
    if(has_register(entry.reg))
    {
      bind(std::span{&entry, 1});
    }
  }
  for(auto const& peripheral : ctx.part.peripherals)
  {
    if(peripheral.alias_of)
    {
      continue;
    }
    if(auto const found = reset_bit_of(desc, peripheral.name))
    {
      resets_.push_back(
          {.instance = peripheral.name,
           .reg      = found->reg,
           .bit      = field(reset_registers[found->reg], found->field)});
    }
  }
  if(has_register("RCC_SECCFGR"))
  {
    bind(rcc_secure_writes);
    secure_ = reg("RCC_SECCFGR");
  }
  if(ctx.trustzone)
  {
    add_security_rules();
  }
  for(auto const& [index, names] : std::views::enumerate(oscillators))
  {
    osc_[index] = {
        .on     = field(names.reg, names.on),
        .ready  = field(names.reg, names.ready),
        .flag   = field("RCC_CIFR", names.flag),
        .enable = field("RCC_CIER", names.enable)};
  }
  for(std::size_t index = 0; index < pll_.size(); ++index)
  {
    auto const n   = std::to_string(index + 1);
    auto const cfg = "RCC_PLL" + n + "CFGR";
    auto const div = "RCC_PLL" + n + "DIVR";
    auto const pll = "PLL" + n;
    pll_[index]    = {
        .src           = field(cfg, pll + "SRC"),
        .range         = field(cfg, pll + "RGE"),
        .m             = field(cfg, pll + "M"),
        .fracen        = field(cfg, pll + "FRACEN"),
        .n             = field(div, pll + "N"),
        .frac          = field("RCC_PLL" + n + "FRACR", pll + "FRACN"),
        .divider       = {field(div, pll + "P"),   field(div, pll + "Q"),   field(div, pll + "R")  },
        .output_enable = {
                          field(cfg, pll + "PEN"), field(cfg, pll + "QEN"), field(cfg, pll + "REN")}
    };
  }
  for(auto const& [index, name] : std::views::enumerate(reset_flags))
  {
    flags_[index] = field("RCC_CSR", name);
  }
  auto& clock = ctx.clock;
  template for(constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^signal)))
  {
    if constexpr([:e:] != none and [:e:] != hsi48_half)
    {
      signals_.emplace_back(clock.domain(std::meta::identifier_of(e)), [:e:]);
    }
  }
  auto const present = [&](std::string_view instance) {
    return std::ranges::contains(ctx.part.peripherals, instance, &peripheral_desc::name);
  };
  for(auto const& entry : muxes)
  {
    for(auto const& user : entry.consumers)
    {
      if(not user.instance.empty() and present(user.instance))
      {
        kernels_.push_back(
            {.domain = clock.domain(lower(user.instance)),
             .select = field(entry.reg, entry.field),
             .inputs = &entry.inputs,
             .enable = enable_field(user.enable)});
      }
    }
  }
  for(auto const& entry : timers)
  {
    if(present(entry.instance))
    {
      timers_.push_back(
          {.domain    = clock.domain(lower(entry.instance)),
           .bus       = entry.bus,
           .prescaler = ppre_[entry.bus == pclk1 ? 0 : 1],
           .enable    = enable_field(entry.enable)});
    }
  }
}

auto rcc::enable_field(std::string_view name) const -> field_ref
{
  for(auto const reg_name : enable_registers)
  {
    if(std::ranges::contains(describe(reg_name).fields, name, &field_desc::name))
    {
      return field(reg_name, name);
    }
  }
  return field(enable_registers.back(), name);
}

auto rcc::is_ready(source which) const -> bool
{
  return get(regs_, osc_[std::to_underlying(which)].ready) != 0;
}

auto rcc::is_on(source which) const -> bool
{
  return get(regs_, osc_[std::to_underlying(which)].on) != 0
      or (which == source::lsi and state_.lsi_forced != 0)
      or (which == source::hsi16 and state_.hsi16_held != 0);
}

auto rcc::pll_input(std::size_t pll) const -> source
{
  constexpr std::array<source, 4> inputs{source::pll1, source::msis, source::hsi16, source::hse};
  return inputs[get(regs_, pll_[pll].src)];
}

auto rcc::system_source(std::uint32_t code) const -> source
{
  constexpr std::array<source, 4> inputs{source::msis, source::hsi16, source::hse, source::pll1};
  return inputs[code];
}

auto rcc::in_use() const -> std::uint32_t
{
  auto const system = system_source(get(regs_, sws_));
  auto       used   = std::uint32_t{1} << std::to_underlying(system);
  if(system == source::pll1 and pll_input(0) != source::pll1)
  {
    used |= std::uint32_t{1} << std::to_underlying(pll_input(0));
  }
  return used;
}

void rcc::schedule(std::uint32_t kind)
{
  auto const generation = ++state_.generation[kind];
  state_.pending |= std::uint32_t{1} << kind;
  auto const delay = static_cast<std::uint64_t>(options_.ready_delay.count()) * 1000u;
  ctx_.events.schedule_at(vtime{ctx_.clock.time().ps + delay}, kind, generation, ctx_.clock);
}

void rcc::cancel(std::uint32_t kind)
{
  ++state_.generation[kind];
  state_.pending &= ~(std::uint32_t{1} << kind);
}

void rcc::start(source which)
{
  auto const index = std::to_underlying(which);
  if(index >= std::to_underlying(source::pll1))
  {
    return;
  }
  schedule(index);
}

void rcc::stop(source which)
{
  auto const index = std::to_underlying(which);
  update(regs_, osc_[index].ready, 0);
  cancel(index);
}

void rcc::oscillators_written(
    std::string_view reg,
    std::uint32_t    word,
    std::uint32_t    old,
    std::uint32_t    now)
{
  auto const used = in_use();
  for(auto const& [index, names] : std::views::enumerate(oscillators))
  {
    if(names.reg != reg)
    {
      continue;
    }
    auto const& osc = osc_[index];
    now             = osc.ready.with(now, osc.ready.of(old));
    if((used >> static_cast<std::uint32_t>(index) & 1u) != 0)
    {
      if(osc.on.of(now) == 0)
      {
        refuse_core_stop(reg, std::format("{} = 0", names.on));
      }
      now = osc.on.with(now, 1);
    }
  }
  regs_.poke(word, now);
  for(auto const& [index, names] : std::views::enumerate(oscillators))
  {
    if(names.reg != reg)
    {
      continue;
    }
    auto const was = osc_[index].on.of(old) != 0;
    auto const is  = osc_[index].on.of(now) != 0;
    if(is and not was)
    {
      start(static_cast<source>(index));
    }
    else if(was and not is and not is_on(static_cast<source>(index)))
    {
      stop(static_cast<source>(index));
    }
  }
  settle(true);
}

void rcc::cr_written(std::uint32_t old, std::uint32_t now)
{
  now |= old & css_on_.mask();
  oscillators_written("RCC_CR", css_on_.offset, old, now);
}

void rcc::icscr1_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(msi_select_.offset, now | (old & msi_select_.mask()));
  settle(true);
}

void rcc::tree_written(std::uint32_t, std::uint32_t)
{
  settle(true);
}

template <std::size_t Pll> void rcc::pll_config_written(std::uint32_t old, std::uint32_t now)
{
  auto const& fields    = pll_[Pll];
  auto const  attempted = now;
  if(locked(Pll))
  {
    for(auto const& kept : {fields.src, fields.range, fields.m})
    {
      now = kept.with(now, kept.of(old));
    }
  }
  auto const drives_core = Pll == 0 and (in_use() >> std::to_underlying(source::pll1) & 1u) != 0;
  if(drives_core)
  {
    now = fields.output_enable[2].with(now, 1);
    if(now != attempted)
    {
      refuse_core_stop("RCC_PLL1CFGR", "a PLL1 configuration change");
    }
  }
  regs_.poke(fields.src.offset, now);
  settle(true);
}

template <std::size_t Pll> void rcc::pll_dividers_written(std::uint32_t old, std::uint32_t now)
{
  auto const& fields    = pll_[Pll];
  auto const  attempted = now;
  if(locked(Pll))
  {
    for(auto const& kept : {fields.n, fields.divider[0], fields.divider[1], fields.divider[2]})
    {
      now = kept.with(now, kept.of(old));
    }
  }
  if(Pll == 0 and now != attempted and (in_use() >> std::to_underlying(source::pll1) & 1u) != 0)
  {
    refuse_core_stop("RCC_PLL1DIVR", "a PLL1 divider change");
  }
  regs_.poke(fields.n.offset, now);
  settle(true);
}

void rcc::refuse_core_stop(std::string_view reg, std::string_view what)
{
  if(std::exchange(core_stop_logged_, true))
  {
    return;
  }
  warn(
      reg,
      std::format(
          "firmware tried to stop the core clock with {}, refused: the core clock stays at {} Hz",
          what, ctx_.clock.core_hz()));
}

auto rcc::locked(std::size_t pll) const -> bool
{
  auto const which = static_cast<source>(std::to_underlying(source::pll1) + pll);
  return is_on(which) or is_ready(which);
}

void rcc::bdcr_written(std::uint32_t old, std::uint32_t now)
{
  if(backup_reset_.of(now) != 0)
  {
    now = backup_reset_.with(0, 1);
  }
  else if(rtc_select_.of(old) != 0)
  {
    now = rtc_select_.with(now, rtc_select_.of(old));
  }
  oscillators_written("RCC_BDCR", bdcr_.offset, old, now);
  if(backup_reset_.of(old) == 0 and backup_reset_.of(now) != 0)
  {
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const domain = dynamic_cast<backup_domain*>(peer))
      {
        domain->backup_reset();
      }
    }
  }
}

void rcc::csr_written(std::uint32_t old, std::uint32_t now)
{
  if(get(regs_, msi_select_) == 0)
  {
    now = msis_standby_range_.with(now, msis_standby_range_.of(old));
    now = msik_standby_range_.with(now, msik_standby_range_.of(old));
  }
  if(remove_flags_.of(now) != 0)
  {
    for(auto const& flag : flags_)
    {
      now = flag.with(now, 0);
    }
  }
  regs_.poke(csr_.offset, remove_flags_.with(now, 0));
}

void rcc::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind > switch_kind or payload != state_.generation[kind] or (state_.pending >> kind & 1u) == 0)
  {
    return;
  }
  state_.pending &= ~(std::uint32_t{1} << kind);
  if(kind == switch_kind)
  {
    auto const target = get(regs_, sw_);
    if(is_ready(system_source(target)))
    {
      update(regs_, sws_, target);
    }
  }
  else if(is_on(static_cast<source>(kind)))
  {
    update(regs_, osc_[kind].ready, 1);
    if(get(regs_, osc_[kind].enable) != 0)
    {
      update(regs_, osc_[kind].flag, 1);
    }
  }
  settle(true);
}

void rcc::settle(bool notify)
{
  for(std::size_t pll = 0; pll < pll_.size(); ++pll)
  {
    auto const which = static_cast<source>(std::to_underlying(source::pll1) + pll);
    auto const index = std::to_underlying(which);
    auto const input = pll_input(pll);
    auto const fed   = input != source::pll1 and is_ready(input);
    if(is_ready(which) and not fed)
    {
      stop(which);
    }
    else if(is_on(which) and not is_ready(which) and fed and (state_.pending >> index & 1u) == 0)
    {
      schedule(index);
    }
  }
  auto const target = get(regs_, sw_);
  if(target == get(regs_, sws_))
  {
    if((state_.pending >> switch_kind & 1u) != 0)
    {
      cancel(switch_kind);
    }
  }
  else if(is_ready(system_source(target)) and (state_.pending >> switch_kind & 1u) == 0)
  {
    schedule(switch_kind);
  }
  publish(notify);
}

auto rcc::rates() const -> std::array<std::uint64_t, signal_count>
{
  std::array<std::uint64_t, signal_count> hz{};
  auto const at     = [&](signal which) -> std::uint64_t& { return hz[std::to_underlying(which)]; };
  auto const ready  = [&](source which) { return is_ready(which); };
  auto const ranged = get(regs_, msi_select_) != 0;
  at(msis) =
      ready(source::msis) ? msi_hz[get(regs_, ranged ? msis_range_ : msis_standby_range_)] : 0;
  at(msik) =
      ready(source::msik) ? msi_hz[get(regs_, ranged ? msik_range_ : msik_standby_range_)] : 0;
  at(hsi16)      = ready(source::hsi16) ? hsi16_hz : 0;
  at(hsi48)      = ready(source::hsi48) ? hsi48_hz : 0;
  at(hsi48_half) = at(hsi48) / 2;
  at(hse)        = ready(source::hse) ? options_.hse_hz : 0;
  at(lse)        = ready(source::lse) ? options_.lse_hz : 0;
  at(lsi)        = ready(source::lsi) ? (get(regs_, lsi_prediv_) != 0 ? lsi_hz / 128 : lsi_hz) : 0;
  constexpr std::array<signal, 4> pll_sources{none, msis, hsi16, hse};
  for(std::size_t pll = 0; pll < pll_.size(); ++pll)
  {
    auto const& fields = pll_[pll];
    auto const  which  = static_cast<source>(std::to_underlying(source::pll1) + pll);
    auto const  input  = at(pll_sources[get(regs_, fields.src)]);
    auto const  frac   = get(regs_, fields.fracen) != 0 ? get(regs_, fields.frac) : 0u;
    auto const  vco    = ready(which) ? input
                                            * ((get(regs_, fields.n) + 1u) * frac_one + frac)
                                            / ((get(regs_, fields.m) + 1u) * frac_one)
                                      : 0;
    for(std::size_t output = 0; output < 3; ++output)
    {
      hz[std::to_underlying(pll1_p) + 3 * pll + output] =
          get(regs_, fields.output_enable[output]) != 0
              ? vco / (get(regs_, fields.divider[output]) + 1u)
              : 0;
    }
  }
  constexpr std::array<signal, 4> system_sources{msis, hsi16, hse, pll1_r};
  at(sysclk) = at(system_sources[get(regs_, sws_)]);
  at(hclk)   = at(sysclk) / hpre_divider(get(regs_, hpre_));
  at(pclk1)  = at(hclk) / ppre_divider(get(regs_, ppre_[0]));
  at(pclk2)  = at(hclk) / ppre_divider(get(regs_, ppre_[1]));
  at(pclk3)  = at(hclk) / ppre_divider(get(regs_, ppre_[2]));
  constexpr std::array<signal, 4> iclk_sources{hsi48, pll2_q, pll1_q, msik};
  at(iclk) = at(iclk_sources[get(regs_, iclk_select_)]);
  std::array<std::uint64_t, 4> const rtc_hz{0, at(lse), at(lsi), at(hse) / 32};
  at(rtc) = get(regs_, rtc_enable_) != 0 ? rtc_hz[get(regs_, rtc_select_)] : 0;
  return hz;
}

void rcc::publish(bool notify)
{
  auto const hz      = rates();
  auto&      clock   = ctx_.clock;
  bool       changed = false;
  auto const set     = [&](core::clock_domain domain, std::uint64_t value) {
    if(clock.hz(domain) != value)
    {
      clock.set_hz(domain, value);
      changed = true;
    }
  };
  for(auto const& [domain, which] : signals_)
  {
    set(domain, hz[std::to_underlying(which)]);
  }
  auto const known = kernel_enables_.size() == kernels_.size();
  kernel_enables_.resize(kernels_.size());
  for(auto const& [entry, published] : std::views::zip(kernels_, kernel_enables_))
  {
    auto const code    = get(regs_, entry.select);
    auto const from    = code < entry.inputs->size() ? (*entry.inputs)[code] : none;
    auto const enabled = get(regs_, entry.enable) != 0 ? std::uint8_t{1} : std::uint8_t{0};
    set(entry.domain, enabled != 0 ? hz[std::to_underlying(from)] : 0);
    changed   = changed or (known and published != enabled);
    published = enabled;
  }
  for(auto const& entry : timers_)
  {
    auto const bus = hz[std::to_underlying(entry.bus)];
    auto const x2  = ppre_divider(get(regs_, entry.prescaler)) == 1 ? 1u : 2u;
    set(entry.domain, get(regs_, entry.enable) != 0 ? bus * x2 : 0);
  }
  auto const core = hz[std::to_underlying(hclk)];
  if(not options_.fixed_core_clock and core == 0)
  {
    refuse_core_stop("RCC_CFGR1", "a stopped system clock");
  }
  else if(not options_.fixed_core_clock and core != clock.core_hz())
  {
    clock.set_core_hz(core);
  }
  if(notify and changed)
  {
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const listener = dynamic_cast<clock_listener*>(peer))
      {
        listener->on_clock_change();
      }
    }
  }
}

void rcc::reset(register_file& regs, reset_kind kind)
{
  auto const kept_bdcr  = regs.peek(bdcr_.offset);
  auto       kept_flags = std::uint32_t{};
  for(auto const& flag : flags_)
  {
    kept_flags |= regs.peek(csr_.offset) & flag.mask();
  }
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto csr = describe("RCC_CSR").reset;
  if(kind != reset_kind::power_on)
  {
    regs.poke(bdcr_.offset, kept_bdcr);
    for(auto const& flag : flags_)
    {
      csr = flag.with(csr, 0);
    }
    csr |= kept_flags;
  }
  for(auto const name : flags_of(kind))
  {
    if(not name.empty())
    {
      csr = field("RCC_CSR", name).with(csr, 1);
    }
  }
  regs.poke(csr_.offset, csr);
  for(auto& generation : state_.generation)
  {
    ++generation;
  }
  state_.pending    = 0;
  state_.lsi_forced = 0;
  state_.hsi16_held = 0;
  if(not is_on(source::lsi))
  {
    update(regs_, osc_[std::to_underlying(source::lsi)].ready, 0);
  }
  publish(false);
}

void rcc::cicr_written(std::uint32_t, std::uint32_t now)
{
  apply_crosses(flag_clears_, now);
  regs_.poke(reg("RCC_CICR").offset, 0);
}

void rcc::add_security_rules()
{
  flag_clears_          = crosses_from("RCC_CICR");
  privilege_            = reg("RCC_PRIVCFGR");
  secure_privileged_    = field("RCC_PRIVCFGR", "SPRIV");
  nonsecure_privileged_ = field("RCC_PRIVCFGR", "NSPRIV");
  for(auto const& group : rcc_secured)
  {
    auto const& described = describe(group.reg);
    for(auto const name : std::views::split(group.fields, ' '))
    {
      auto const wanted = std::string_view{name};
      if(std::ranges::contains(described.fields, wanted, &field_desc::name))
      {
        secured_.emplace_back(field("RCC_SECCFGR", group.bit), field(group.reg, wanted));
      }
    }
  }
  for(auto const name : rcc_unit_registers)
  {
    for(auto const& bits : describe(name).fields)
    {
      auto const suffix = std::ranges::find_if(
          rcc_unit_suffixes, [&](std::string_view s) { return bits.name.ends_with(s); });
      if(suffix == rcc_unit_suffixes.end())
      {
        continue;
      }
      auto stem = std::string_view{bits.name}.substr(0, bits.name.size() - suffix->size());
      if(auto const alias = std::ranges::find(rcc_unit_aliases, stem, &unit_alias::first);
         alias != rcc_unit_aliases.end())
      {
        stem = alias->second;
      }
      if(auto const unit = unit_named(ctx_.part, stem))
      {
        units_.push_back(
            {.offset = reg(name).offset, .bits = field(name, bits.name).mask(), .unit = *unit});
      }
    }
  }
  for(auto const& name : claims())
  {
    std::uint32_t mask = 0;
    for(auto const& bits : describe(name).fields)
    {
      mask |= field(name, bits.name).mask();
    }
    words_.emplace_back(reg(name).offset, mask);
  }
}

void rcc::resolve_units()
{
  if(units_resolved_ or ctx_.peers.all().empty())
  {
    return;
  }
  units_resolved_ = true;
  for(auto* const peer : ctx_.peers.all())
  {
    auto* const firewall = dynamic_cast<peripheral_security*>(peer);
    if(firewall != nullptr and firewall_ == nullptr)
    {
      firewall_ = firewall;
    }
  }
  for(auto& rule : units_)
  {
    rule.aware = dynamic_cast<trustzone_aware const*>(
        ctx_.peers.find(ctx_.part.peripherals[rule.unit].name));
  }
  backup_units_ = {
      dynamic_cast<trustzone_aware const*>(ctx_.peers.find("RTC")),
      dynamic_cast<trustzone_aware const*>(ctx_.peers.find("TAMP"))};
}

auto rcc::unit_is_secure(unit_bits const& rule) const -> bool
{
  if(firewall_ != nullptr)
  {
    if(auto const secure = firewall_->peripheral_secure(rule.unit))
    {
      return *secure;
    }
  }
  return rule.aware != nullptr and rule.aware->secure_function();
}

auto rcc::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone and secure_ ? regs_.peek(secure_->offset) : 0;
}

auto rcc::secure_function() const -> bool
{
  return secure_word() != 0;
}

auto rcc::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  auto const all = ~std::uint32_t{};
  if(secure_ and word == secure_->offset)
  {
    if(not ctx_.trustzone)
    {
      return bus_guard::both(all);
    }
    if(not ctx.secure)
    {
      if(write and unit_)
      {
        illegal_.raise(ctx_.peers, *unit_, ctx.by);
      }
      return {.wi = all};
    }
    auto const spriv = secure_privileged_.of(regs_.peek(privilege_.offset)) != 0;
    return {.wi = not ctx.privileged and spriv ? all : 0};
  }
  if(not ctx_.trustzone)
  {
    return {};
  }
  auto const privilege = regs_.peek(privilege_.offset);
  if(word == privilege_.offset)
  {
    if(not ctx.privileged)
    {
      return {.wi = all};
    }
    return {.wi = ctx.secure ? 0 : secure_privileged_.mask()};
  }
  resolve_units();
  auto const    seccfgr = secure_word();
  std::uint32_t secured = 0;
  for(auto const& [bit, bits] : secured_)
  {
    if(bits.offset == word and bit.of(seccfgr) != 0)
    {
      secured |= bits.mask();
    }
  }
  for(auto const& rule : units_)
  {
    if(rule.offset == word and unit_is_secure(rule))
    {
      secured |= rule.bits;
    }
  }
  if(word == bdcr_.offset and std::ranges::any_of(backup_units_, [](trustzone_aware const* unit) {
       return unit != nullptr and unit->secure_function();
     }))
  {
    secured |= backup_reset_.mask();
  }
  auto const known =
      std::ranges::find(words_, word, &std::pair<std::uint32_t, std::uint32_t>::first);
  auto const whole = known != words_.end() ? known->second : all;
  return guard_of(secured, true, secure_privileged_.of(privilege) != 0, ctx)
       | guard_of(whole & ~secured, false, nonsecure_privileged_.of(privilege) != 0, ctx);
}

void rcc::force_lsi()
{
  auto const lsi    = source::lsi;
  auto const was    = is_on(lsi);
  state_.lsi_forced = 1;
  if(not was and not is_ready(lsi))
  {
    start(lsi);
  }
}

void rcc::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  kernel_enables_.clear();
  for(auto const& entry : kernels_)
  {
    kernel_enables_.push_back(get(regs_, entry.enable) != 0 ? 1 : 0);
  }
}

auto rcc::bus_enabled(core::clock_domain kernel) const -> bool
{
  auto const found = std::ranges::find(kernels_, kernel, &kernel::domain);
  return found == kernels_.end() or get(regs_, found->enable) != 0;
}

auto rcc::reset_line(std::string_view instance) const -> std::optional<std::size_t>
{
  auto const found = std::ranges::find(resets_, instance, &reset_entry::instance);
  if(found == resets_.end())
  {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - resets_.begin());
}

auto rcc::held_in_reset(std::size_t line) const -> bool
{
  return line < resets_.size() and get(regs_, resets_[line].bit) != 0;
}

template <std::size_t Reg> void rcc::resets_written(std::uint32_t old, std::uint32_t now)
{
  for(auto const& entry : resets_)
  {
    if(entry.reg != Reg or entry.bit.of(old) != 0 or entry.bit.of(now) == 0)
    {
      continue;
    }
    if(auto* const listener = dynamic_cast<bus_reset_listener*>(ctx_.peers.find(entry.instance)))
    {
      listener->on_bus_reset();
    }
  }
}

void rcc::hold_hsi16(bool held)
{
  auto const hsi    = source::hsi16;
  auto const was    = is_on(hsi);
  state_.hsi16_held = held ? 1 : 0;
  auto const is     = is_on(hsi);
  if(is and not was and not is_ready(hsi))
  {
    start(hsi);
  }
  else if(was and not is and (in_use() >> std::to_underlying(hsi) & 1u) == 0)
  {
    stop(hsi);
    settle(true);
  }
}
}

auto rcc_factory(rcc_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rcc>(ctx, desc, options);
  };
}
}
