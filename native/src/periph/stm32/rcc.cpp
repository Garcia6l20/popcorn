#include "backup_domain.hpp"
#include "family_names.hpp"
#include "flash_option.hpp"
#include "gtzc.hpp"
#include "hsi16_source.hpp"
#include "kernel_gate.hpp"
#include "lsi_source.hpp"
#include "model_base.hpp"
#include "rcc_enables.hpp"
#include "rcc_h5.hpp"
#include "rcc_l1.hpp"
#include "rcc_l4.hpp"
#include "rcc_resets.hpp"
#include "rcc_rows.hpp"
#include "rcc_u5.hpp"
#include "secure_config.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/clock_output.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <meta>
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
template <typename Traits>
inline constexpr std::size_t source_count = role_count<typename Traits::source>;

template <typename Traits>
inline constexpr std::size_t signal_count = role_count<typename Traits::signal>;

template <typename Traits>
inline constexpr std::size_t dependent_count = [] {
  if constexpr(requires { Traits::dependent_clocks; })
  {
    return Traits::dependent_clocks.size();
  }
  else
  {
    return std::size_t{};
  }
}();

template <typename Traits>
inline constexpr std::size_t output_count = [] {
  if constexpr(requires { Traits::clock_outputs; })
  {
    return Traits::clock_outputs.size();
  }
  else
  {
    return std::size_t{};
  }
}();

template <typename Traits>
inline constexpr std::size_t first_dependent_event =
    source_count<Traits> + (Traits::hsi_divider ? 1 : 0);

template <typename Traits>
inline constexpr std::size_t event_count = first_dependent_event<Traits> + dependent_count<Traits>;

struct unit_bits
{
  std::uint32_t          offset{};
  std::uint32_t          bits{};
  std::size_t            unit{};
  trustzone_aware const* aware{};
};

/**
 * @brief Whether each oscillator row of @p Traits sits at the index of its source: a row with an
 *        output signal names the signal of its source's name.
 */
template <typename Traits> consteval auto oscillators_in_source_order() -> bool
{
  using signal       = typename Traits::signal;
  auto const sources = std::meta::enumerators_of(std::meta::dealias(^^typename Traits::source));
  auto const signals = std::meta::enumerators_of(std::meta::dealias(^^signal));
  if(sources.size() != Traits::oscillators.size())
  {
    return false;
  }
  for(std::size_t index = 0; index < sources.size(); ++index)
  {
    auto const out = Traits::oscillators[index].out;
    if(out == signal::none)
    {
      continue;
    }
    auto const named = std::ranges::find_if(
        signals, [&](std::meta::info e) { return std::meta::extract<signal>(e) == out; });
    if(std::meta::identifier_of(*named) != std::meta::identifier_of(sources[index]))
    {
      return false;
    }
  }
  return true;
}

/**
 * @brief Whether the parallel source maps of @p Traits agree: each SYSCLK and PLL input names
 *        the source whose oscillator outputs the signal of the same code, PLL1 exactly for a
 *        PLL1 output, a reserved SYSCLK code exactly for `none`, and the PLL input `pll1`
 *        exactly for the PLL source `none`.
 */
template <typename Traits> consteval auto source_maps_agree() -> bool
{
  using source       = typename Traits::source;
  using signal       = typename Traits::signal;
  auto const& pll1   = Traits::pll_nodes.front();
  auto const  output = [](source which) {
    return Traits::oscillators[std::to_underlying(which)].out;
  };
  auto const from_pll1 = [&](signal which) {
    return std::ranges::contains(pll1.out.begin(), pll1.out.begin() + pll1.outputs, which);
  };
  for(std::size_t code = 0; code < Traits::system_inputs.size(); ++code)
  {
    auto const input = Traits::system_inputs[code];
    auto const out   = Traits::system_sources[code];
    if(not input)
    {
      if(out != signal::none)
      {
        return false;
      }
    }
    else if(
        (*input == source::pll1) != from_pll1(out)
        or (*input != source::pll1 and output(*input) != out))
    {
      return false;
    }
  }
  if(Traits::pll_inputs.size() != Traits::pll_sources.size())
  {
    return false;
  }
  for(std::size_t code = 0; code < Traits::pll_inputs.size(); ++code)
  {
    auto const input = Traits::pll_inputs[code];
    auto const out   = Traits::pll_sources[code];
    if((input == source::pll1) != (out == signal::none)
       or (input != source::pll1 and output(input) != out))
    {
      return false;
    }
  }
  return true;
}

struct output_note
{
  std::uint64_t id{};
  std::uint64_t upto{};
  std::uint64_t at_ps{};
  std::uint64_t hz{};
  std::uint8_t  output{};

  friend auto operator==(output_note const&, output_note const&) -> bool = default;
};

struct output_watch
{
  std::uint64_t                                                        id{};
  std::size_t                                                          output{};
  std::shared_ptr<std::move_only_function<void(std::uint64_t, vtime)>> call;
};

struct output_table
{
  std::uint64_t             next{};
  std::vector<output_watch> entries;
};

template <typename Traits> struct rcc_state
{
  std::array<std::uint32_t, event_count<Traits>>      generation{};
  std::uint32_t                                       pending{};
  std::uint8_t                                        lsi_forced{};
  std::array<std::uint32_t, Traits::pll_nodes.size()> fraction{};
  std::uint32_t                                       hsi_divider{};
  std::uint8_t                                        divider_clearing{};
  std::uint64_t                                       divider_set_ps{};
  std::vector<output_note>                            notes;
  std::uint64_t                                       next_note{};

  friend auto operator==(rcc_state const&, rcc_state const&) -> bool = default;
};

struct oscillator
{
  field_ref on;
  field_ref ready;
  field_ref flag;
  field_ref enable;
};

struct dependent_fields
{
  field_ref enable;
  field_ref ready;
};

template <typename Traits>
using dependent_array = std::array<dependent_fields, dependent_count<Traits>>;

struct output_fields
{
  field_ref enable;
  field_ref select;
};

struct pll_fields
{
  field_ref                                             src;
  std::optional<field_ref>                              range;
  std::optional<field_ref>                              m;
  std::optional<field_ref>                              fracen;
  std::optional<field_ref>                              vcosel;
  field_ref                                             n;
  std::optional<field_ref>                              frac;
  std::array<field_ref, max_pll_outputs>                divider{};
  std::array<std::optional<field_ref>, max_pll_outputs> output_enable{};
};

template <typename Signal> struct kernel
{
  core::clock_domain           domain;
  field_ref                    select;
  std::array<Signal, 8> const* inputs;
  field_ref                    enable;
};

struct reset_entry
{
  std::string instance;
  std::size_t reg;
  field_ref   bit;
};

struct enable_entry
{
  std::string      instance;
  std::string_view reg;
  field_ref        bit;
};

template <typename Signal> struct timer
{
  core::clock_domain domain;
  Signal             bus;
  rcc_role           prescaler;
  field_ref          enable;
};

template <typename Signal> struct bus_gate
{
  core::clock_domain domain;
  Signal             bus;
  field_ref          enable;
};

/**
 * @brief RCC of the STM32U5, RM0456 section 11, and of the STM32H5, RM0481 section 11. Ready flags
 *        follow their enables after the board's ready delay. SWS follows SW in the write that
 *        selects a ready source, or when the selected source becomes ready, RM0456 11.4.9 and
 *        RM0481 11.4.8. RCC writes RCC_CIFR storage directly when the matching RCC_CIER bit is set
 *        and raises no ready interrupt: firmware waiting on the RCC IRQ hangs, and the spin stop
 *        names its loop. RCC holds, with no log record, each write RM0456 11.8 and RM0481 11.8 say
 *        the hardware ignores: a clear of the ON bit of the SYSCLK source and of the PLL1 input
 *        while PLL1 drives SYSCLK, on the STM32H5 of the PLL1 input while PLL1ON stays set after
 *        the write, RM0481 11.8.1, a write of `PLLxSRC`, `PLLxM`, `PLLxRGE`, `PLLxN` and the output
 *        dividers while the PLL is on or ready, on the STM32H5 of PLL1PEN, PLL1QEN and PLL1REN too,
 *        RM0481 11.8.7, and a clear of the PLL1 output enable that feeds SYSCLK while it does. The
 *        STM32U5 `PLL1MBOOST` is storage. A selected SYSCLK source that runs at 0 Hz, such as a
 *        PLL1 output whose enable is clear, leaves the core clock at its rate and logs one warning
 *        per machine life. A started IWDG keeps the LSI on until the next reset, RM0456 section
 *        11.4.16. FLASH programming or erase runs HSI16 for the FLASH interface, RM0456 sections
 *        7.3.6 and 7.3.7, and `HSIRDY` and the HSI16 clock of the tree follow `HSION` alone, RM0456
 *        11.8.1. A change of an instance's bit in an RCC_xxxRSTR register, RM0456 11.8.21 to
 *        11.8.28, holds or releases the instance through `core::machine_services::hold_in_reset()`,
 *        which resets its storage and calls its model's
 *        `periph::bus_reset_listener::on_bus_reset()`. The bit is `<instance>RST`, or the name of
 *        `reset_bit_names`. The STM32H5 reset registers are RM0481 11.8.19 to 11.8.25, its reset
 *        flags `RCC_RSR`, RM0481 11.8.46. An instance's bus clock enable gates its register
 *        access through `core::machine_services::gate_bus_clock()`: CPU and DMA accesses to it
 *        read 0 and lose their writes while the bit is 0. The bit is `<instance>EN`, or the name
 *        of `enable_bit_names`, in an RCC_xxxENR register, or the RTCEN of the STM32L1 RCC_CSR
 *        and STM32L4 RCC_BDCR for the RTC. The RCC drives every gate when it is built, from the
 *        description reset values, after each machine reset and restore, from the registers, and
 *        on every write that changes the bit. An instance without such a bit, such as EXTI,
 *        IWDG or DBGMCU, is never gated. Read 0 and write lost are inferred: the reference
 *        manuals say only that such an access is not supported.
 *
 *        The PLL fractional modulator loads `PLLxFRACN` when `PLLxON` sets with `PLLxFRACEN` 1,
 *        and 0 with `PLLxFRACEN` 0, in either write order, as measured on an STM32H573. While
 *        the PLL runs, a `PLLxFRACEN` write from 0 to 1 loads `PLLxFRACN` again and the VCO keeps
 *        the loaded fraction whatever `PLLxFRACEN` and `PLLxFRACN` read later, RM0456 11.4.6 and
 *        11.8.9, RM0481 11.4.5 and 11.8.7. Deviation: the fraction applies at once, silicon takes
 *        it three reference clock periods later.
 *
 *        STM32H5, RM0481 11.8.1: `HSIDIV` reads its new value from the write. `HSIDIVF` clears and
 *        then sets again with the new HSI rate after the cycles of the running HSI rate in
 *        `h5_rcc::hsi_divider_timings`, as measured on an STM32H573: /2 to /4 or /8 clear at 3 and
 *        set at 6, /2 to /1 3 and 16, /4 to /2 2 and 8. The HSI keeps its old rate until `HSIDIVF`
 *        sets, a point the board run did not measure. `HSIDIV` keeps its value while PLL1 drives
 *        SYSCLK from HSI and changes in every other state, an enabled PLL1 off SYSCLK fed by HSI
 *        included, as measured on an STM32H573 against the RM0481 11.8.1 text. For PLL2 and PLL3
 *        fed by HSI the model applies the same rule, inferred from PLL1. `TIMPRE`, RM0481 11.8.5,
 *        sets the timer clocks of APB1 and APB2. `VSWRST` resets the backup domain, RM0481 11.3.3.
 *
 *        TrustZone, RM0456 11.5 and Table 117: the bits each `RCC_SECCFGR` bit secures, and the
 *        reset, enable, sleep enable, autonomous enable and kernel clock selection bits of a
 *        Secure peripheral, are RAZ/WI to a Non-secure access, with no event. A peripheral is
 *        Secure by its TZSC bit, or for a TrustZone-aware model by its own security bits, the
 *        FLASH by RM0456 Table 62. A bit shared by several peripherals, such as `ADCDACSEL` or
 *        `LPTIM34SEL`, is Secure while any of them is, as "its related clock source selection"
 *        of RM0456 11.5.1 and RM0481 11.5.1 reads for each of them. On the STM32U5 `TIMICSEL`
 *        follows TIM16 and TIM17 only, a Secure LPTIM2 leaving it Non-secure, ES0499 2.3.2.
 *        The internal clocks the timer input captures divide are published as the domains
 *        `timic_hsi`, `timic_csi` (STM32H5), `timic_msi_1024` and `timic_msi_4` (STM32U5): the
 *        undivided rate of the oscillator the tap divides, 1024 and 4 being the tap names, 0 while
 *        the most significant bit of `RCC_CCIPR1.TIMICSEL` is 0, RM0456 11.8.46 and RM0481 11.8.40.
 *        On the STM32U5 bit 1 of TIMICSEL takes the 1024 tap from MSIK, bit 0 the 4 tap, else from
 *        MSIS, the codes 100 to 111 of RM0456 11.8.46. TIMICSEL 001 to 011 are closed, inferred
 *        from the register text, RM0456 11.4 reads the gate as TIMICSEL[2:0] not 0.
 *        A bit whose name gives no peripheral of the part stays Non-secure, such as the GTZCn.
 *        Deviation: SRAMnEN and SRAMnSMEN stay Non-secure, the MPCBB and INVSECSTATE rule of
 *        RM0456 5.4 is not modelled. `BDRST` is Secure while the RTC or the TAMP has a secure
 *        function, RM0456 11.5.1. The STM32H5 follows RM0481 11.5 and Table 122 the same way,
 *        through the H5 traits, the sleep enables in `RCC_xxxLPENR`, and `RTCPRE`, `RTCEN` and
 *        `RTCSEL` Secure in place of `BDRST`. `SPRIV` makes the Secure bits privileged,
 *        `NSPRIV` the others, RAZ/WI to an unprivileged access.
 *        `RCC_SECCFGR` reads for every transaction and takes Secure writes, privileged with SPRIV
 *        set. A Non-secure write to it is ignored with an illegal access event. `RCC_PRIVCFGR`
 *        takes privileged writes, SPRIV Secure ones only. A write of `RCC_CICR` clears the
 *        `RCC_CIFR` flags of the description's cross rules, the flags of a Secure oscillator only
 *        for a Secure write. Under TZEN=0 `RCC_SECCFGR` is RAZ/WI, RM0456 11.8.51, and
 *        `RCC_PRIVCFGR` is storage.
 *
 *        STM32U5 LSE system clock, RM0456 11.4.7 and 11.8.49: `LSESYSRDY` sets two LSE periods
 *        after `LSESYSEN` with `LSEON` and `LSERDY` set, and clears at once with `LSESYSEN`,
 *        `LSEON` or `LSERDY`. The `lse_system` signal runs at the LSE rate while `LSESYSRDY` is
 *        set and at 0 Hz otherwise. The USART, LPUART and LPTIM kernel clock muxes read the LSE
 *        through it, and the RTC clock reads the LSE directly. `BDRST` keeps `LSESYSEN`, and
 *        `LSESYSRDY` follows the LSE that `BDRST` stops. The trait `dependent_clocks` holds such a
 *        clock: its enable and ready bits, its source oscillator and its delay in source periods.
 *        The STM32U5 `MSIPLLEN` reads 0 while `LSERDY` is 0 and clears with the LSE, RM0456 11.8.1.
 *
 *        The low-speed clock output, RM0456 11.4.22, RM0351 6.2.17 and RM0481 11.4.11: `LSCOEN`
 *        drives LSI, or LSE with `LSCOSEL`, onto PA2 on the STM32U5 and STM32L4 and PB2 on the
 *        STM32H5, DS13086, DS10198, DS10199 and DS14121 pin tables. The STM32U5 LSE source is the
 *        `lse_system` signal, so it needs `LSESYSRDY`. The rate is 0 while the source is off or
 *        `LSCOEN` is clear, and does not depend on the pin's GPIO mode, since the manuals leave
 *        the pin in its reset state. `clock_output_port::clock_hz()` reads the rate and
 *        `watch_clock()` calls a watcher once per rate change at the change time.
 *
 *        STM32L1, RM0038 section 6, through `l1_rcc`: one PLL in RCC_CFGR, whose PLLSRC, PLLMUL
 *        and PLLDIV hold while it is on or ready, RM0038 6.3.3, the ready flags and enables in
 *        RCC_CIR, the LSI, the LSE, the RTC clock and the reset flags in RCC_CSR. CSSON is set and
 *        cleared by software, RM0038 6.3.1. A system reset keeps the RTC domain bits of RCC_CSR
 *        and RTCPRE and HSEBYP of RCC_CR, a power on reset and a BOR reset them, RM0038 6.1.2 and
 *        6.1.3. RTCRST holds the RTC domain bits of RCC_CSR at 0 and keeps LSION, LSIRDY and the
 *        reset flags. A PLLMUL, PLLDIV or MSIRANGE code RM0038 does not allow runs at 0 Hz.
 *
 *        STM32L47x/L48x, RM0351 section 6, through `l4_rcc`, whose class doc lists its rules.
 *        The traits `writable_msi_ranges` and `lse_locked_controls` add the MSIRANGE write
 *        protection and the LSE lock of MSIPLLEN, RM0351 6.4.1. The three PLLs share the PLLSRC and
 *        PLLM of RCC_PLLCFGR, which keep their value while any PLL that reads them is on or
 *        ready. The trait `backup_reset_kept` names the RCC_BDCR fields BDRST keeps, LSCOSEL
 *        and LSCOEN, RM0351 6.4.29, and on the U5 RM0456 11.8.49.
 * @tparam Traits Family clock tree, u5_rcc, h5_rcc, l1_rcc or l4_rcc: the sources and signals,
 *         the oscillator, mux, timer and enable tables, the source maps of the PLLs, SYSCLK and
 *         RTC, the names of the single fields, and the TrustZone tables.
 *
 * @limits rcc RCC
 *
 * @group all rule "Clock gates"
 * - A peripheral's bus clock enable, `<instance>EN` or its RM spelling in an `RCC_xxxENR` register,
 *   gates its register access: while the bit is 0 a CPU or DMA read gives 0 and a write is lost, no
 *   model handler runs, one `model_warning` per peripheral per machine life names the first such
 *   access, and under `strict_peripherals` the access raises `model_error`. Debugger accesses are
 *   served. Read 0 and write lost are inferred: RM0456, RM0481, RM0351 and RM0038 say only that the
 *   access is not supported. The enable gates no activity of the peripheral: its timers, transfers
 *   and events run on. A gated write reaches no model check, such as an illegal-access event of a
 *   TrustZone-aware model, inferred.
 * - The STM32L1 and STM32L4 RTC is gated on `RTCEN` of `RCC_CSR` or `RCC_BDCR`, which a system
 *   reset keeps, as RM0038 5.1.3 and RM0351 5.1.5 list it among the steps of RTC register access.
 *   The STM32U5 and STM32H5 RTC and TAMP are gated on `RTCAPBEN`.
 * - Instances whose clock the reference manual names only through their host block are gated on the
 *   host's bit, inferred: the GTZC TZIC and MPCBB blocks on the GTZC or TZSC enable, the OCTOSPI
 *   delay blocks on the OCTOSPI enable, the STM32U5 SDMMC delay blocks on the SDMMC enable, STM32U5
 *   FDCAN1_RAM on `FDCAN1EN`, STM32L1 OPAMP on `COMPEN` and STM32L1 USB_SRAM on `USBEN`. The
 *   STM32H563 and STM32H573 ETH is gated on `ETHEN` alone, `ETHTXEN` and `ETHRXEN` are storage,
 *   inferred.
 * - RCC, EXTI, IWDG and DBGMCU, the STM32H5 PWR and the STM32U5 and STM32H5 ICACHE have no enable
 *   and are never gated. FLASH and the STM32U5 and STM32H5 DCACHE enables reset to 1.
 * - SRAMnEN and SRAMnSMEN stay Non-secure. The MPCBB and INVSECSTATE rule of RM0456 5.4 does not
 *   apply to them.
 *
 * @group all rule "Peripheral resets"
 * - A peripheral reset bit in an `RCC_xxxRSTR` register, written from 0 to 1, resets every register
 *   of that instance that no model claims, for any instance, modelled or not, such as LPTIM1. It
 *   also resets the instance's model when the model supports it: RNG, CRC, HASH, SYSCFG, ADC1 and
 *   ADC12 through ADC12RST on the STM32U5, ADC1, ADC2 and ADCC on the STM32H5, GPDMA1, LPDMA1, the
 *   timers, GPIOA to GPIOI, SPI, I2C, SAI, MDF1 and ADF1 on the STM32U5, the USARTs, UARTs, LPUART1
 *   on the STM32U5 and LPUART on the STM32H5, OTG_FS on the STM32U5 and USB on the STM32H5. A
 *   native plugin model that derives from `periph::bus_reset_listener`, and every Python model,
 *   hears it through `on_bus_reset()`. Any other model gets `reset()` with
 *   `reset_kind::peripheral`. A peripheral reset leaves the flags of RCC_CSR, and of RCC_RSR on the
 *   STM32H5, as they are.
 * - Board state survives such a reset: a pin's board drive, SPI devices and I2C targets, which an
 *   RCC reset of their controller does not reset, PDM sources, SAI external clocks and the frames a
 *   test queued for a USART receiver.
 * - A USART reset mid-frame aborts the frame on its UartLink, as clearing UE does. Frames the host
 *   queued wait until firmware enables the receiver again.
 * - An OTG_FS reset releases the pull-up, so the board's USB host sees a detach and enumerates the
 *   device again once firmware reconnects. The cable stays plugged or unplugged as it was.
 * - A GPIO reset returns its pins to their reset modes, analog except PA13 to PA15, PB3 and PB4, so
 *   outputs drop and listeners see the edges. A pin in analog mode reads low whatever the board
 *   drives, so an active-low SPI chip select on a reset port selects its device until firmware
 *   configures the pin again.
 * - SYSCFGRST keeps `SYSCFG_CSLCKR`, `SYSCFG_CNSLCKR` and `SYSCFG_CFGR2`, whose bits RM0456 15.3
 *   clears only on a system reset.
 * - While a reset bit is 1, bus writes to that peripheral are ignored, from the CPU, DMA and
 *   debugger alike. The TrustZone filter still runs first, so an illegal access still raises its
 *   event. Reads are not blocked. RM0456 gives no access rule for a peripheral held in reset.
 *
 * @group u5,h5 rule "TrustZone"
 * - TrustZone: an RCC bit shared by several peripherals is Secure while any of them is, on the
 *   STM32U5 and the STM32H5 alike. RM0456 11.5.1 and RM0481 11.5.1 secure "its related clock source
 *   selection" for each Secure peripheral and name no rule for a shared one. The shared bits are
 *   `ADCDACSEL`, `LPTIM34SEL`, `OCTOSPISEL`, `SDMMCSEL` and `TIMICSEL` on the STM32U5, `ADCDACSEL`,
 *   `TIMICSEL` and the `FDCAN12` bits on the STM32H5. The STM32U5 `TIMICSEL` follows TIM16 and
 *   TIM17 only: a Secure LPTIM2 leaves it Non-secure, ES0499 2.3.2.
 * - STM32H5 TrustZone: `RCC_CR.HSIDIV`, `HSIDIVF` and the `RCC_CFGR2` `AHBxDIS` and `APBxDIS` bits
 *   take Non-secure writes, RM0481 Table 122 lists them under no security bit.
 *
 * @group all rule "Clock selection and PLL"
 * - SWS follows SW in the store that selects a ready source. On an STM32H573, the first one or two
 *   reads of `RCC_CFGR1` after that store return the old SWS. Firmware that polls SWS ends in the
 *   same state.
 * - RCC holds each write RM0456 11.8 and RM0481 11.8 say the hardware ignores, with no log record,
 *   on the STM32U5 and the STM32H5 alike: a clear of the ON bit of the SYSCLK source and of the
 *   PLL1 input while PLL1 drives SYSCLK, on the STM32H5 a clear of HSEON, HSION or CSION while that
 *   oscillator feeds PLL1 and the same write leaves PLL1ON set, RM0481 11.8.1, a write of
 *   `PLLxSRC`, `PLLxM`, `PLLxRGE`, `PLLxN` and the output dividers while the PLL is on or ready, on
 *   the STM32H5 of PLL1PEN, PLL1QEN and PLL1REN too, RM0481 11.8.7, and a clear of the PLL1 output
 *   enable that feeds SYSCLK. The STM32U5 `PLL1MBOOST` is storage. ST's CMSIS `SystemInit` run with
 *   PLL1 as SYSCLK logs nothing.
 * - A switch to a SYSCLK source that runs at 0 Hz, such as a PLL1 output whose enable is clear,
 *   keeps the core clock at its last rate and logs one `model_warning` per machine.
 * - A PLL loads `PLLxFRACN` as its fraction when `PLLxON` sets with `PLLxFRACEN` 1, and runs in
 *   integer mode with `PLLxFRACEN` 0, whatever order firmware wrote them in, as measured on an
 *   STM32H573. While the PLL runs, it takes a new `PLLxFRACN` only on a `PLLxFRACEN` write from 0
 *   to 1 and keeps the loaded fraction across a `PLLxFRACEN` clear. That running rule follows
 *   RM0456 11.4.6 and RM0481 11.4.5, unmeasured. The new fraction applies at once, where silicon
 *   takes it three reference clock periods later.
 *
 * @group u5,h5 rule "PLL limits"
 * - STM32U5: a PLL checks its reference clock against 4 to 16 MHz, RM0456 11.4.6, and its VCO and
 *   enabled outputs against the VCORE range, Table 115: VCO 128 to 544 MHz with outputs up to 208
 *   MHz in range 1 and 110 MHz in range 2, VCO 128 to 330 MHz with outputs up to 55 MHz in range
 *   3. A PLL is not allowed in range 4, the range after a reset, RM0456 10.5.2.
 * - STM32H5: the limits follow `PLLxVCOSEL`, the wide and medium VCO range tables of the PLL
 *   characteristics in the datasheet, the same in the datasheet of every shipped part. The wide VCO
 * range, 0, takes a reference clock of 2 to 16 MHz, a VCO of 128 to 560 MHz and outputs up to 250,
 * 200, 150 and 100 MHz in VOS0 to VOS3. The medium range, 1, takes 1 to 2 MHz, 150 to 420 MHz and
 * outputs up to 210, 210, 160 and 88 MHz. VOS3 is the range after a reset, RM0481 10.5.2.
 * - A PLL enabled outside its limits logs one model warning per PLL per machine life, naming the
 *   numbers, and the PLL still locks. Under `strict_peripherals` the write that sets `PLLxON`
 *   raises `model_error` and `PLLxON` reads 0. A firmware VOS write that lowers the range under a
 *   running PLL is checked against the range it selects and logs the same warning at the write.
 *   RM0456 10.5.4 reduces the system frequency before VOS is lowered, and the RM0481 10.5 example
 *   disables the PLL before VOS3. Under `strict_peripherals` the PWR write raises
 *   `model_error` and the register keeps its whole word, an inferred response, the manuals do not
 *   say what silicon does when VOS is lowered under a PLL above the limits. A `PLLxON` write
 *   between a lowering VOS write and the new range applying is checked against the lower
 *   performance of the two ranges, inferred. The outputs and dividers read are those of the
 *   `PLLxON` write and of the range change. The manuals say the PLL output is not guaranteed
 *   outside the limits and do not say `PLLxRDY` stays low, inferred.
 *
 * @group h5 rule "STM32H5 HSIDIV"
 * - STM32H5: `RCC_CR.HSIDIV` keeps its value while PLL1 drives SYSCLK from HSI and takes a write in
 *   every other state, an enabled PLL1 off SYSCLK fed by HSI included, as measured on an STM32H573
 *   for PLL1. For PLL2 and PLL3 fed by HSI the model applies the same rule, inferred from PLL1. One
 *   write that clears PLL1ON of a PLL1 off SYSCLK and changes `HSIDIV` takes both.
 * - STM32H5: after an `HSIDIV` change, `HSIDIV` reads the new value at once. `HSIDIVF` clears and
 *   sets again after the cycles of the running HSI rate measured on an STM32H573: /2 to /4 or /8
 *   clear at 3 and set at 6, /2 to /1 3 and 16, /4 to /2 2 and 8, /4 to /1 2 and 14, /8 to /1 2 and
 *   8. The other transitions are inferred: from /1 they take the /2 row, /4 to /8 and /8 to /2 and
 *   /4 the /4 to /2 timing. The new HSI rate applies when `HSIDIVF` sets, a point the board run did
 *   not measure. On an STM32H573 ST's HAL reads `HSIDIVF` about 24 cycles after the store and reads
 *   1 for every transition. The model runs one cycle per instruction, so the same load lands 13
 *   cycles after the store and reads 1, except for /2 to /1 and /4 to /1, where it reads 0 and HAL
 *   takes `HSI_VALUE` either way.
 *
 * @group all rule "Oscillators and interrupt flags"
 * - STM32U5 and STM32H5: `HSEBYP` in `RCC_CR` and `LSEBYP` in `RCC_BDCR` take writes while their
 *   oscillator runs, where RM0456 and RM0481 allow a write with the oscillator off only.
 * - An oscillator whose ON bit a system reset keeps and whose ready bit is 0, such as an LSE still
 *   starting when the reset comes, starts again at the reset and is ready after the board's
 *   `ready_delay`.
 * - Without TrustZone, a coverage report classes `RCC_CIFR` and `RCC_CICR` as `unimplemented`. The
 *   RCC sets the `RCC_CIFR` flags `RCC_CIER` enables, and a `RCC_CICR` write clears them.
 *
 * @group l1 rule "STM32L1"
 * - STM32L1: every category runs one clock tree, RM0038 section 6, with the enable, reset and
 *   low-power bits of a peripheral the category lacks absent from its description. The RCC sets the
 *   `RCC_CIR` ready flags its enables select, a `xxxRDYC` write clears them, and no ready interrupt
 *   is raised. A system reset keeps the RTC domain bits of `RCC_CSR`, `RTCPRE` and `HSEBYP`, a
 *   power on reset and a BOR reset them, RM0038 6.1.2 and 6.3.14. A BOR sets PORRSTF and PINRSTF,
 *   inferred. `RTCRST` resets the RTC domain bits of `RCC_CSR` while it is 1. The LSE CSS bits
 *   exist on the L100, L151 and L162 from Cat.2, and OBLRSTF on every line but the L152, RM0038
 *   6.3.4 and 6.3.14 against the SVDs, inferred. A software reset sets SFTRSTF only, the PINRSTF of
 *   the NRST pulse is not set, an inferred rule. The reset flags of `RCC_CSR` keep their value on a
 *   write, only RMVF and a power reset clear them, inferred from RM0038 6.3.14.
 * - STM32L1: `RTCPRE` and `HSEBYP` keep their value on a write while `HSEON` is 1 and `LSEBYP`
 *   while `LSEON` is 1, RM0038 6.3.1 and 6.3.14. The ON bit is the one before the write, so one
 *   store that clears `HSEON` and changes `RTCPRE` keeps `RTCPRE`, an inferred order. `RTCRST`
 *   clears `LSEBYP` while the LSE runs, RM0038 6.3.14.
 * - STM32L1: `MSIRANGE` 7 and the PLLMUL and PLLDIV codes RM0038 6.3.2 and 6.3.3 do not allow run
 *   their output at 0 Hz, an inferred rule. The PLL VCO and SYSCLK limits of each voltage range are
 *   not checked. The CSS on HSE and on LSE is storage, and `LSECSSON` can be cleared by firmware.
 *   The USB and SDIO 48 MHz clock, PLLVCO / 2, is not a clock domain.
 *
 * @group l4 rule "STM32L47x/L48x"
 * - STM32L47x/L48x: one clock tree, RM0351 section 6. The RCC sets the `RCC_CIFR` ready flags that
 *   `RCC_CIER` enables, a `RCC_CICR` write clears them, and no ready interrupt is raised. Each MSI
 *   range runs at its nominal rate, with or without `MSIPLLEN`, and HSI16 at 16 MHz: the
 *   `RCC_ICSCR` trim fields are storage. A ready flag clears in the write that clears its ON bit,
 *   where RM0351 6.4.1 and 6.4.30 give 6 oscillator cycles, 3 for the LSI.
 * - STM32L47x/L48x: a write of `MSIRANGE` while `MSION` is 1 and `MSIRDY` is 0, which RM0351 6.4.1
 *   forbids, takes the new range. `LSEBYP` takes every write, RM0351 6.4.29 states no rule for it.
 *   `HSIKERON`, `HSIASFS` and `STOPWUCK` are storage, they act on Stop mode entry and exit. The CSS
 *   on HSE and on LSE and `LSEDRV` are storage, `CSSF`, `LSECSSF` and `LSECSSD` stay 0. `RCC_BDCR`
 *   takes writes whatever `PWR_CR1.DBP` holds.
 * - STM32L47x/L48x: the main PLL, PLLSAI1 and PLLSAI2 run at the rate their PLLSRC, PLLM, N and
 *   output dividers give, RM0351 6.4.4 to 6.4.6, for any VCO input and output: the 4 to 16 MHz
 *   input and 64 to 344 MHz output ranges and the 80 MHz output limits are the firmware's to keep.
 *   A PLL is ready after the board's `ready_delay`. The PLLSAI1 and PLLSAI2 outputs reach no kernel
 *   clock until the `RCC_CCIPR` muxes select them.
 * - STM32L47x/L48x: `ADCRST` holds ADC1, ADC2, ADC3 and ADC_Common in reset together, RM0351
 *   6.4.11. The enable bits gate the register access of their peripherals, the timer clocks and the
 *   kernel clocks `RCC_CCIPR` selects. `MCOSEL` and `MCOPRE` are storage, no clock reaches the MCO
 *   pin. The sleep and stop enables of `RCC_xxxSMENR` are storage.
 * - STM32L47x/L48x: `SAIxSEL` 11 selects `SAIx_EXTCLK`, which runs the SAI kernel clock at 0 Hz.
 *   `LSCOSEL` and `LSCOEN` keep their value through `BDRST`, RM0351 6.4.29. `FWRSTF` stays 0, the
 *   firewall raises no reset.
 *
 * @group u5 rule "STM32U5 LSE"
 * - STM32U5 LSE system clock, RM0456 11.4.7 and 11.8.49: the LSE reaches the RTC and TAMP at
 *   `LSERDY`. It reaches the USART, UART, LPUART and LPTIM kernel clock muxes and the LSCO output
 *   only after `LSESYSEN` is set and `LSESYSRDY` follows, two LSE periods after `LSESYSEN` with
 *   `LSERDY` set. A kernel clock that selects the LSE before `LSESYSRDY` runs at 0 Hz. `LSESYSRDY`
 *   clears with `LSESYSEN`, `LSEON` and `LSERDY`, and `BDRST` keeps `LSESYSEN`. `LSERDY` clears at
 *   once when `LSEON` clears, where RM0456 gives six LSE cycles.
 * - STM32U5 `MSIPLLEN` reads 0 while `LSERDY` is 0 and clears when the LSE stops, RM0456 11.8.1.
 *   The clear on a CSS-on-LSE failure is not modelled, `LSECSSON` is storage.
 *
 * @group all rule "LSCO and the LSI rate"
 * - LSCO: `LSCOEN` drives the LSI, or the LSE with `LSCOSEL`, onto PA2 on the STM32U5 and STM32L4
 *   and onto PB2 on the STM32H5, RM0456 11.4.22, RM0351 6.2.17 and RM0481 11.4.11. A board device
 *   reads the rate with `DeviceBoard.pin_clock` and follows its changes with
 *   `DeviceBoard.watch_pin_clock`. The rate is 0 while the source is off. The rate does not depend
 *   on the GPIO mode of the pin, inferred: RM0351 6.2.17 and RM0481 11.4.11 only say the LSCO pin
 *   is left in its reset state, and RM0456 is silent. A low-power mode entry does not change the
 *   rate, where RM0456 11.4.22 and RM0351 6.2.17 keep the LSCO running in Standby and RM0481
 *   11.4.11 does not. The STM32L1 has no LSCO.
 * - `RccOptions.lsi_hz` sets the LSI rate in Hz before the STM32U5 `LSIPREDIV` division, the family
 *   nominal when unset: 32 kHz on the STM32U5, STM32H5 and STM32L4, 37 kHz on the STM32L1. The
 *   datasheet full-range limits bound it, 30.4 to 33.6 kHz on the STM32U5, 29.4 to 33.6 kHz on the
 *   STM32H5, 29.5 to 34 kHz on the STM32L4 and 26 to 56 kHz on the STM32L1.
 *
 * @group l4 note "STM32L47x/L48x clock errata"
 * - STM32L47x/L48x clock errata, ES0250 2.2: `MSIRDY` clears whenever the MSI stops, 2.2.1. The MSI
 *   runs at its range rate from the first cycle after a wakeup, 2.2.3 and 2.2.24. The LSI runs at
 *   32 kHz with a 50 % duty, 2.2.8. The HSE is ready after the board's `ready_delay` at any VDD,
 *   2.2.10. The HSI16 user trim is storage, 2.2.22. A PLL locks at any VCO rate and temperature,
 *   2.2.25. The LSE runs at the board rate whatever PC13 does, 2.2.11.
 */
template <typename Traits>
class rcc_model final : public dispatch_model<rcc_model<Traits>, rcc_state<Traits>>,
                        public lsi_source,
                        public hsi16_source,
                        public kernel_gate,
                        public clock_output_port,
                        public voltage_range_listener,
                        public voltage_range_guard,
                        public trustzone_aware
{
  using base   = dispatch_model<rcc_model<Traits>, rcc_state<Traits>>;
  using source = typename Traits::source;
  using signal = typename Traits::signal;

  static constexpr std::size_t   source_count = stm32::source_count<Traits>;
  static constexpr std::size_t   signal_count = stm32::signal_count<Traits>;
  static constexpr std::uint32_t divider_kind = source_count;

  static_assert(Traits::oscillators.size() == source_count);
  static_assert(std::to_underlying(source::pll1) + Traits::pll_nodes.size() == source_count);
  static_assert(std::to_underlying(signal::none) == 0);
  static_assert(Traits::system_inputs.size() == Traits::sw_mask + 1);
  static_assert(Traits::system_sources.size() == Traits::sw_mask + 1);
  static_assert(oscillators_in_source_order<Traits>());
  static_assert(source_maps_agree<Traits>());
  static_assert(flag_matches_roles(
      Traits::names,
      Traits::hsi_divider,
      {rcc_role::hsi_divider, rcc_role::hsi_divider_flag}));
  static_assert(std::ranges::all_of(Traits::timers, [](auto const& entry) {
    return std::ranges::contains(Traits::buses, entry.bus, &bus_node<signal>::out);
  }));

  static constexpr std::size_t system_output = [] {
    auto const& node = Traits::pll_nodes.front();
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      if(std::ranges::contains(Traits::system_sources, node.out[output]))
      {
        return output;
      }
    }
    return max_pll_outputs;
  }();
  static_assert(system_output < max_pll_outputs, "a PLL1 output feeds SYSCLK");

  using base::apply_crosses;
  using base::bind;
  using base::crosses_from;
  using base::ctx_;
  using base::describe;
  using base::field;
  using base::has_field;
  using base::has_register;
  using base::reg;
  using base::state_;
  using base::warn;

public:
  rcc_model(model_context& ctx, peripheral_desc const& desc, rcc_options const& options);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto drives_core_clock() const noexcept -> bool override
  {
    return true;
  }
  void               force_lsi() override;
  void               on_voltage_range_change() override;
  [[nodiscard]] auto voltage_range_refused(std::uint32_t level) -> bool override;
  void               hold_hsi16(bool held) override;
  [[nodiscard]] auto bus_enabled(core::clock_domain kernel) const -> bool override;
  [[nodiscard]] auto clock_hz(std::string_view port, std::uint8_t pin) const
      -> std::uint64_t override;
  [[nodiscard]] auto watch_clock(
      std::string_view                                          port,
      std::uint8_t                                              pin,
      std::move_only_function<void(std::uint64_t hz, vtime at)> watcher) -> subscription override;

  void               cr_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto divider_written(std::uint32_t old, std::uint32_t now) -> std::uint32_t;
  void               icscr1_written(std::uint32_t old, std::uint32_t now);
  void               tree_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Pll> void pll_config_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Pll> void pll_dividers_written(std::uint32_t old, std::uint32_t now);
  void                            bdcr_written(std::uint32_t old, std::uint32_t now);
  void                            csr_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto flags_written(std::uint32_t old, std::uint32_t now) const -> std::uint32_t;
  [[nodiscard]] auto held(std::uint32_t word, std::uint32_t old, std::uint32_t now) const
      -> std::uint32_t;
  void                            cicr_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Reg> void resets_written(std::uint32_t old, std::uint32_t now);
  auto                            on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void               add_security_rules();
  void               resolve_units();
  [[nodiscard]] auto unit_is_secure(unit_bits const& rule) const -> bool;

  [[nodiscard]] auto is_ready(source which) const -> bool;
  [[nodiscard]] auto is_on(source which) const -> bool;
  [[nodiscard]] auto pll_input(std::size_t pll) const -> source;
  [[nodiscard]] auto system_source(std::uint32_t code) const -> std::optional<source>;
  [[nodiscard]] auto in_use() const -> std::uint32_t;
  [[nodiscard]] auto locked(std::size_t pll) const -> bool;
  [[nodiscard]] auto input_locked(std::size_t pll) const -> bool;
  [[nodiscard]] auto rates() const -> std::array<std::uint64_t, signal_count>;
  [[nodiscard]] auto enable_field(std::string_view name) const -> field_ref;
  [[nodiscard]] auto read(rcc_role role) const -> std::uint32_t;
  [[nodiscard]] auto pll_violations(std::size_t pll, std::uint32_t level) const -> std::string;
  [[nodiscard]] auto worst_level() const -> std::optional<std::uint32_t>;
  void               check_pll_enable(
      std::string_view reg,
      std::uint32_t    word,
      std::uint32_t    old,
      std::uint32_t    now);

  void oscillators_written(
      std::string_view reg,
      std::uint32_t    word,
      std::uint32_t    old,
      std::uint32_t    now);
  void start(source which);
  void load_fraction(std::size_t pll);
  void stop(source which);
  void schedule(std::uint32_t kind, std::optional<std::uint64_t> delay_ps = std::nullopt);
  void cancel(std::uint32_t kind);
  void settle(bool notify);
  void follow_dependents();
  void follow_outputs(std::array<std::uint64_t, signal_count> const& hz, bool notify);
  void post_output(std::size_t index, std::uint64_t rate);
  [[nodiscard]] auto output_index(std::string_view port, std::uint8_t pin) const -> std::size_t;
  void               publish(bool notify);
  void               load(std::span<std::byte const> in) override;
  void               refuse_stopped_clock();
  void               enables_written(std::uint32_t word, std::uint32_t old, std::uint32_t now);
  void               drive_enables(bool from_reset_values);
  void               sleep_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto wwdg_hardware() const -> bool;

  bool                                            core_stop_logged_{};
  std::array<bool, Traits::pll_nodes.size()>      pll_range_logged_{};
  rcc_options                                     options_;
  register_file&                                  regs_;
  std::array<oscillator, source_count>            osc_{};
  dependent_array<Traits>                         dependents_{};
  std::array<output_fields, output_count<Traits>> outputs_{};
  std::array<std::uint64_t, output_count<Traits>> output_hz_{};
  std::shared_ptr<output_table> output_watchers_ = std::make_shared<output_table>();
  std::array<pll_fields, Traits::pll_nodes.size()>     pll_{};
  field_table<rcc_role>                                names_;
  std::array<field_ref, Traits::signal_muxes.size()>   selects_{};
  std::array<field_ref, Traits::reset_flags.size()>    flags_{};
  reg_ref                                              control_;
  reg_ref                                              bdcr_;
  std::uint32_t                                        backup_mask_{};
  std::uint32_t                                        backup_reset_mask_{};
  reg_ref                                              csr_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> kept_;
  std::vector<std::pair<field_ref, source>>            held_;
  std::vector<field_ref>                               lse_locked_;
  std::vector<std::pair<core::clock_domain, signal>>   signals_;
  std::vector<kernel<signal>>                          kernels_;
  std::vector<reset_entry>                             resets_;
  std::vector<enable_entry>                            enables_;
  std::vector<std::string>                             reset_registers_;
  std::vector<std::uint8_t>                            kernel_enables_;
  std::vector<timer<signal>>                           timers_;
  std::vector<bus_gate<signal>>                        bus_gates_;
  struct timic_entry
  {
    core::clock_domain domain;
    signal             clear;
    signal             set;
    std::int8_t        select_bit;
  };
  field_ref                                            timic_;
  std::vector<timic_entry>                             timic_taps_;
  std::optional<option_bits>                           wwdg_option_;
  field_ref                                            wwdg_enable_;
  field_ref                                            wwdg_sleep_;
  std::optional<reg_ref>                               secure_;
  reg_ref                                              privilege_;
  field_ref                                            secure_privileged_;
  field_ref                                            nonsecure_privileged_;
  std::vector<std::pair<field_ref, field_ref>>         secured_;
  std::vector<unit_bits>                               units_;
  std::vector<cross_ref>                               flag_clears_;
  std::array<trustzone_aware const*, 2>                backup_units_{};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> backup_secured_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> words_;
  peripheral_security*                                 firewall_{};
  bool                                                 units_resolved_{};
  std::optional<std::size_t>                           unit_;
  illegal_event                                        illegal_;
};

template <typename Traits> struct rcc_tables;

template <> struct rcc_tables<u5_rcc>
{
  using model = rcc_model<u5_rcc>;

  static constexpr std::array<write_entry<model>, 28> writes{
      {
       {"RCC_CR", &model::cr_written},
       {"RCC_ICSCR1", &model::icscr1_written},
       {"RCC_CFGR1", &model::tree_written},
       {"RCC_CFGR2", &model::tree_written},
       {"RCC_CFGR3", &model::tree_written},
       {"RCC_CIER", nullptr},
       {"RCC_PLL1CFGR", &model::pll_config_written<0>},
       {"RCC_PLL2CFGR", &model::pll_config_written<1>},
       {"RCC_PLL3CFGR", &model::pll_config_written<2>},
       {"RCC_PLL1DIVR", &model::pll_dividers_written<0>},
       {"RCC_PLL2DIVR", &model::pll_dividers_written<1>},
       {"RCC_PLL3DIVR", &model::pll_dividers_written<2>},
       {"RCC_PLL1FRACR", &model::tree_written},
       {"RCC_PLL2FRACR", &model::tree_written},
       {"RCC_PLL3FRACR", &model::tree_written},
       {"RCC_CCIPR1", &model::tree_written},
       {"RCC_CCIPR2", &model::tree_written},
       {"RCC_CCIPR3", &model::tree_written},
       {"RCC_AHB1ENR", &model::tree_written},
       {"RCC_AHB2ENR1", &model::tree_written},
       {"RCC_AHB2ENR2", &model::tree_written},
       {"RCC_AHB3ENR", &model::tree_written},
       {"RCC_APB1ENR1", &model::tree_written},
       {"RCC_APB1ENR2", &model::tree_written},
       {"RCC_APB2ENR", &model::tree_written},
       {"RCC_APB3ENR", &model::tree_written},
       {"RCC_BDCR", &model::bdcr_written},
       {"RCC_CSR", &model::csr_written},
       }
  };

  static constexpr auto trustzone_writes = std::to_array<write_entry<model>>({
      {"RCC_ICSCR2",     nullptr             },
      {"RCC_ICSCR3",     nullptr             },
      {"RCC_CRRCR",      nullptr             },
      {"RCC_CIFR",       nullptr             },
      {"RCC_CICR",       &model::cicr_written},
      {"RCC_AHB1SMENR",  nullptr             },
      {"RCC_AHB2SMENR1", nullptr             },
      {"RCC_AHB2SMENR2", nullptr             },
      {"RCC_AHB3SMENR",  nullptr             },
      {"RCC_APB1SMENR1", nullptr             },
      {"RCC_APB1SMENR2", nullptr             },
      {"RCC_APB2SMENR",  nullptr             },
      {"RCC_APB3SMENR",  nullptr             },
      {"RCC_SRDAMR",     nullptr             },
      {"RCC_PRIVCFGR",   nullptr             },
  });
};

template <> struct rcc_tables<h5_rcc>
{
  using model = rcc_model<h5_rcc>;

  static constexpr std::array<write_entry<model>, 27> writes{
      {
       {"RCC_CR", &model::cr_written},
       {"RCC_CFGR1", &model::tree_written},
       {"RCC_CFGR2", &model::tree_written},
       {"RCC_CIER", nullptr},
       {"RCC_PLL1CFGR", &model::pll_config_written<0>},
       {"RCC_PLL2CFGR", &model::pll_config_written<1>},
       {"RCC_PLL3CFGR", &model::pll_config_written<2>},
       {"RCC_PLL1DIVR", &model::pll_dividers_written<0>},
       {"RCC_PLL2DIVR", &model::pll_dividers_written<1>},
       {"RCC_PLL3DIVR", &model::pll_dividers_written<2>},
       {"RCC_PLL1FRACR", &model::tree_written},
       {"RCC_PLL2FRACR", &model::tree_written},
       {"RCC_PLL3FRACR", &model::tree_written},
       {"RCC_CCIPR1", &model::tree_written},
       {"RCC_CCIPR2", &model::tree_written},
       {"RCC_CCIPR3", &model::tree_written},
       {"RCC_CCIPR4", &model::tree_written},
       {"RCC_CCIPR5", &model::tree_written},
       {"RCC_AHB1ENR", &model::tree_written},
       {"RCC_AHB2ENR", &model::tree_written},
       {"RCC_AHB4ENR", &model::tree_written},
       {"RCC_APB1LENR", &model::tree_written},
       {"RCC_APB1HENR", &model::tree_written},
       {"RCC_APB2ENR", &model::tree_written},
       {"RCC_APB3ENR", &model::tree_written},
       {"RCC_BDCR", &model::bdcr_written},
       {"RCC_RSR", &model::csr_written},
       }
  };

  static constexpr auto trustzone_writes = std::to_array<write_entry<model>>({
      {"RCC_HSICFGR",    nullptr             },
      {"RCC_CSICFGR",    nullptr             },
      {"RCC_CRRCR",      nullptr             },
      {"RCC_CIFR",       nullptr             },
      {"RCC_CICR",       &model::cicr_written},
      {"RCC_AHB1LPENR",  nullptr             },
      {"RCC_AHB2LPENR",  nullptr             },
      {"RCC_AHB4LPENR",  nullptr             },
      {"RCC_APB1LLPENR", nullptr             },
      {"RCC_APB1HLPENR", nullptr             },
      {"RCC_APB2LPENR",  nullptr             },
      {"RCC_APB3LPENR",  nullptr             },
      {"RCC_PRIVCFGR",   nullptr             },
  });
};

template <> struct rcc_tables<l1_rcc>
{
  using model = rcc_model<l1_rcc>;

  static constexpr std::array<write_entry<model>, 7> writes{
      {
       {"CR", &model::cr_written},
       {"ICSCR", &model::tree_written},
       {"CFGR", &model::pll_config_written<0>},
       {"AHBENR", &model::tree_written},
       {"APB2ENR", &model::tree_written},
       {"APB1ENR", &model::tree_written},
       {"CSR", &model::bdcr_written},
       }
  };

  static constexpr std::array<write_entry<model>, 0> trustzone_writes{};
};

template <> struct rcc_tables<l4_rcc>
{
  using model = rcc_model<l4_rcc>;

  static constexpr std::array<write_entry<model>, 15> writes{
      {
       {"CR", &model::cr_written},
       {"CFGR", &model::tree_written},
       {"CCIPR", &model::tree_written},
       {"PLLCFGR", &model::pll_config_written<0>},
       {"PLLSAI1CFGR", &model::pll_dividers_written<1>},
       {"PLLSAI2CFGR", &model::pll_dividers_written<2>},
       {"CSR", &model::csr_written},
       {"CIER", nullptr},
       {"AHB1ENR", &model::tree_written},
       {"AHB2ENR", &model::tree_written},
       {"AHB3ENR", &model::tree_written},
       {"APB1ENR1", &model::tree_written},
       {"APB1ENR2", &model::tree_written},
       {"APB2ENR", &model::tree_written},
       {"BDCR", &model::bdcr_written},
       }
  };

  static constexpr std::array<write_entry<model>, 0> trustzone_writes{};
};

constexpr std::size_t max_reset_registers = 16;

template <typename Model, std::size_t... Reg>
constexpr auto reset_handlers(std::index_sequence<Reg...>)
    -> std::array<void (Model::*)(std::uint32_t, std::uint32_t), sizeof...(Reg)>
{
  return {&Model::template resets_written<Reg>...};
}

template <typename Traits>
rcc_model<Traits>::rcc_model(
    model_context&         ctx,
    peripheral_desc const& desc,
    rcc_options const&     options)
    : base{ctx, desc, "RCC", rcc_tables<Traits>::writes, rcc_tables<Traits>::trustzone_writes},
      options_{options}, regs_{ctx.regs},
      names_{
          Traits::names,
          [this](std::string_view reg_name, std::string_view name) {
            return field(reg_name, name);
          },
          [this](std::string_view reg_name, std::string_view name) {
            return has_field(reg_name, name);
          }},
      control_{reg(Traits::control_register)}, bdcr_{reg(Traits::backup_register)},
      csr_{reg(Traits::reset_flags_register)}, unit_{unit_named(ctx.part, desc.name)}
{
  if(options_.lsi_hz
     and (*options_.lsi_hz < Traits::lsi_limits.min or *options_.lsi_hz > Traits::lsi_limits.max))
  {
    throw config_error(
        ctx.part.part, 0, "rcc.lsi_hz",
        std::format(
            "an LSI frequency from {} to {} Hz, not {} Hz", Traits::lsi_limits.min,
            Traits::lsi_limits.max, *options_.lsi_hz));
  }
  for(auto const& [index, entry] : std::views::enumerate(Traits::signal_muxes))
  {
    selects_[index] = field(entry.reg, entry.field);
  }
  backup_mask_ = ~std::uint32_t{};
  for(auto const name : std::views::split(Traits::backup_excluded, ' '))
  {
    backup_mask_ &= ~field(Traits::backup_register, std::string_view{name}).mask();
  }
  for(auto const& group : Traits::system_reset_kept)
  {
    for(auto const name : std::views::split(group.fields, ' '))
    {
      auto const bits = field(group.reg, std::string_view{name});
      kept_.emplace_back(bits.offset, bits.mask());
    }
  }
  for(auto const& group : Traits::held_while_on)
  {
    for(auto const name : std::views::split(group.fields, ' '))
    {
      held_.emplace_back(field(group.reg, std::string_view{name}), group.source);
    }
  }
  for(auto const& described : desc.registers)
  {
    if(not described.alternate_of and described.name.contains("RSTR"))
    {
      reset_registers_.push_back(described.name);
    }
  }
  if(reset_registers_.size() > max_reset_registers)
  {
    throw config_error(
        ctx.part.part, 0, "peripherals.RCC.registers",
        std::format("at most {} registers named RSTR", max_reset_registers));
  }
  constexpr auto handlers =
      reset_handlers<rcc_model>(std::make_index_sequence<max_reset_registers>{});
  std::vector<write_entry<rcc_model>> reset_writes;
  for(std::size_t index = 0; index < reset_registers_.size(); ++index)
  {
    reset_writes.push_back({reset_registers_[index], handlers[index]});
  }
  bind(reset_writes);
  for(auto const& peripheral : ctx.part.peripherals)
  {
    if(peripheral.alias_of)
    {
      continue;
    }
    if(auto const found = reset_bit_of(desc, peripheral.name))
    {
      auto const index = std::ranges::find(reset_registers_, found->reg);
      resets_.push_back(
          {.instance = peripheral.name,
           .reg      = static_cast<std::size_t>(index - reset_registers_.begin()),
           .bit      = field(found->reg, found->field)});
    }
  }
  constexpr auto enable_extra =
      std::to_array<std::string_view>({Traits::reset_flags_register, Traits::backup_register});
  for(auto const& peripheral : ctx.part.peripherals)
  {
    if(peripheral.alias_of)
    {
      continue;
    }
    auto const found = enable_bit_of(desc, peripheral.name, enable_extra);
    if(not found)
    {
      if(has_enable_row(peripheral.name))
      {
        throw config_error(
            ctx.part.part, 0,
            std::format(
                "peripherals.RCC.registers.RCC_xxxENR.fields.{}",
                enable_bit_candidates(peripheral.name)
                    | std::views::join_with(std::string_view{" or "})
                    | std::ranges::to<std::string>()),
            std::format("the RCC bus clock enable of {}", peripheral.name));
      }
      continue;
    }
    auto const& described = describe(found->reg);
    enables_.push_back(
        {.instance = peripheral.name,
         .reg      = described.name,
         .bit      = field(found->reg, found->field)});
  }
  auto const claimed = this->claims();
  for(auto const& entry : enables_)
  {
    if(std::ranges::none_of(
           claimed, [&](std::string const& held) { return reg(held).offset == entry.bit.offset; }))
    {
      throw config_error(
          ctx.part.part, 0, std::format("peripherals.RCC.registers.{}", entry.reg),
          std::format(
              "a register the RCC model claims, for the bus clock enable of {}", entry.instance));
    }
  }
  if(has_register("RCC_SECCFGR"))
  {
    constexpr std::array<write_entry<rcc_model>, 1> secure_writes{
        {
         {"RCC_SECCFGR", nullptr},
         }
    };
    bind(secure_writes);
    secure_ = reg("RCC_SECCFGR");
  }
  if(ctx.trustzone)
  {
    add_security_rules();
  }
  for(auto const& [index, names] : std::views::enumerate(Traits::oscillators))
  {
    osc_[index] = {
        .on     = field(names.reg, names.on),
        .ready  = field(names.reg, names.ready),
        .flag   = field(Traits::flag_register, names.flag),
        .enable = field(Traits::enable_register, names.enable)};
  }
  if constexpr(requires { Traits::dependent_clocks; })
  {
    for(auto const& [index, names] : std::views::enumerate(Traits::dependent_clocks))
    {
      dependents_[index] = {
          .enable = field(names.reg, names.enable), .ready = field(names.reg, names.ready)};
    }
  }
  if constexpr(requires { Traits::clock_outputs; })
  {
    for(auto const& [index, names] : std::views::enumerate(Traits::clock_outputs))
    {
      outputs_[index] = {
          .enable = field(names.reg, names.enable), .select = field(names.reg, names.select)};
    }
  }
  if constexpr(requires { Traits::lse_locked_controls; })
  {
    for(auto const name : Traits::lse_locked_controls)
    {
      lse_locked_.push_back(field(Traits::control_register, name));
    }
  }
  if constexpr(requires { Traits::writable_msi_ranges; })
  {
    if(names_[rcc_role::msis_range].offset != control_.offset)
    {
      throw config_error(
          ctx.part.part, 0, "peripherals.RCC.registers",
          "a write-protected MSI range in the control register");
    }
  }
  for(auto const& [bits, which] : held_)
  {
    if(bits.offset != osc_[std::to_underlying(which)].on.offset)
    {
      throw config_error(
          ctx.part.part, 0, "peripherals.RCC.registers",
          "a field held while an oscillator is on in the register of its ON bit");
    }
  }
  for(auto const& [index, node] : std::views::enumerate(Traits::pll_nodes))
  {
    auto const named = [&](std::string_view reg_name, std::string_view name) {
      return name.empty() ? std::nullopt : std::optional{field(reg_name, name)};
    };
    auto& fields  = pll_[index];
    fields.src    = field(node.config, node.src);
    fields.range  = named(node.config, node.range);
    fields.m      = named(node.config, node.m);
    fields.fracen = named(node.config, node.fracen);
    fields.vcosel = named(node.config, node.vcosel);
    fields.n      = field(node.dividers, node.n);
    fields.frac   = named(node.fraction, node.frac);
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      fields.divider[output] = field(node.dividers, node.divider[output]);
    }
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      fields.output_enable[output] =
          named(node.enables.empty() ? node.config : node.enables, node.output_enable[output]);
    }
  }
  for(auto const& [index, name] : std::views::enumerate(Traits::reset_flags))
  {
    flags_[index] = field(Traits::reset_flags_register, name);
    if(flags_[index].offset == bdcr_.offset)
    {
      backup_mask_ &= ~flags_[index].mask();
    }
  }
  backup_reset_mask_ = backup_mask_;
  if constexpr(requires { Traits::backup_reset_kept; })
  {
    for(auto const name : std::views::split(Traits::backup_reset_kept, ' '))
    {
      backup_reset_mask_ &= ~field(Traits::backup_register, std::string_view{name}).mask();
    }
  }
  auto& clock = ctx.clock;
  template for(constexpr auto e :
               std::define_static_array(std::meta::enumerators_of(std::meta::dealias(^^signal))))
  {
    if constexpr(
        [:e:] != signal::none and not std::ranges::contains(Traits::internal_signals, [:e:]))
    {
      signals_.emplace_back(clock.domain(std::meta::identifier_of(e)), [:e:]);
    }
  }
  auto const present = [&](std::string_view instance) {
    return std::ranges::contains(ctx.part.peripherals, instance, &peripheral_desc::name);
  };
  for(auto const& entry : Traits::muxes)
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
  for(auto const& entry : Traits::timers)
  {
    if(present(entry.instance))
    {
      timers_.push_back(
          {.domain = clock.domain(lower(entry.instance)),
           .bus    = entry.bus,
           .prescaler =
               std::ranges::find(Traits::buses, entry.bus, &bus_node<signal>::out)->prescaler,
           .enable = enable_field(entry.enable)});
    }
  }
  for(auto const& entry : Traits::bus_gated)
  {
    if(present(entry.instance))
    {
      bus_gates_.push_back(
          {.domain = clock.domain(lower(entry.instance)),
           .bus    = entry.bus,
           .enable = enable_field(entry.enable)});
    }
  }
  if(not Traits::timic.reg.empty())
  {
    timic_ = field(Traits::timic.reg, Traits::timic.field);
    for(auto const& tap : Traits::timic_taps)
    {
      timic_taps_.push_back(
          {.domain     = clock.domain(tap.domain),
           .clear      = tap.clear,
           .set        = tap.set,
           .select_bit = tap.select_bit});
    }
  }
  if(not Traits::wwdg_option.reg.empty() and present("WWDG"))
  {
    wwdg_option_ = option_bits_of(
        ctx, Traits::wwdg_option, "the hardware watchdog option the RCC enables the WWDG for");
    wwdg_enable_ = enable_field("WWDGEN");
    if(not Traits::wwdg_sleep.reg.empty())
    {
      wwdg_sleep_ = field(Traits::wwdg_sleep.reg, Traits::wwdg_sleep.field);
      if(not std::ranges::contains(this->claims(), this->describe(Traits::wwdg_sleep.reg).name))
      {
        this->bind(
            std::array<write_entry<rcc_model>, 1>{
                {{Traits::wwdg_sleep.reg, &rcc_model::sleep_written}}});
      }
    }
  }
  drive_enables(true);
}

template <typename Traits> void rcc_model<Traits>::sleep_written(std::uint32_t, std::uint32_t)
{
  if(wwdg_hardware())
  {
    update(regs_, wwdg_sleep_, 1);
  }
}

template <typename Traits> auto rcc_model<Traits>::wwdg_hardware() const -> bool
{
  if(not wwdg_option_)
  {
    return false;
  }
  auto const word = ctx_.services.read(wwdg_option_->at, 4, initiator::debugger);
  return word and (*word & wwdg_option_->mask) == 0;
}

template <typename Traits>
auto rcc_model<Traits>::enable_field(std::string_view name) const -> field_ref
{
  for(auto const reg_name : Traits::enable_registers)
  {
    if(std::ranges::contains(describe(reg_name).fields, name, &field_desc::name))
    {
      return field(reg_name, name);
    }
  }
  return field(Traits::enable_registers.back(), name);
}

template <typename Traits> auto rcc_model<Traits>::read(rcc_role role) const -> std::uint32_t
{
  return get(regs_, names_[role]);
}

template <typename Traits> auto rcc_model<Traits>::is_ready(source which) const -> bool
{
  return get(regs_, osc_[std::to_underlying(which)].ready) != 0;
}

template <typename Traits> auto rcc_model<Traits>::is_on(source which) const -> bool
{
  return get(regs_, osc_[std::to_underlying(which)].on) != 0
      or (which == source::lsi and state_.lsi_forced != 0);
}

template <typename Traits> auto rcc_model<Traits>::pll_input(std::size_t pll) const -> source
{
  return Traits::pll_inputs[get(regs_, pll_[pll].src)];
}

template <typename Traits>
auto rcc_model<Traits>::system_source(std::uint32_t code) const -> std::optional<source>
{
  return Traits::system_inputs[code & Traits::sw_mask];
}

template <typename Traits> auto rcc_model<Traits>::in_use() const -> std::uint32_t
{
  auto const system = system_source(read(rcc_role::sws));
  if(not system)
  {
    return 0;
  }
  auto used = std::uint32_t{1} << std::to_underlying(*system);
  if(*system == source::pll1 and pll_input(0) != source::pll1)
  {
    used |= std::uint32_t{1} << std::to_underlying(pll_input(0));
  }
  return used;
}

template <typename Traits>
void rcc_model<Traits>::schedule(std::uint32_t kind, std::optional<std::uint64_t> delay_ps)
{
  auto const generation = ++state_.generation[kind];
  state_.pending |= std::uint32_t{1} << kind;
  auto const delay =
      delay_ps.value_or(static_cast<std::uint64_t>(options_.ready_delay.count()) * 1000u);
  ctx_.events.schedule_at(vtime{ctx_.clock.time().ps + delay}, kind, generation, ctx_.clock);
}

template <typename Traits> void rcc_model<Traits>::cancel(std::uint32_t kind)
{
  ++state_.generation[kind];
  state_.pending &= ~(std::uint32_t{1} << kind);
}

template <typename Traits> void rcc_model<Traits>::load_fraction(std::size_t pll)
{
  auto const& fields   = pll_[pll];
  auto const  fraction = fields.fracen and fields.frac and get(regs_, *fields.fracen) != 0;
  state_.fraction[pll] = fraction ? get(regs_, *fields.frac) : 0;
}

template <typename Traits> void rcc_model<Traits>::start(source which)
{
  auto const index = std::to_underlying(which);
  if(index >= std::to_underlying(source::pll1))
  {
    return;
  }
  schedule(index);
}

template <typename Traits> void rcc_model<Traits>::stop(source which)
{
  auto const index = std::to_underlying(which);
  update(regs_, osc_[index].ready, 0);
  cancel(index);
  if(which == source::lse)
  {
    for(auto const& bits : lse_locked_)
    {
      update(regs_, bits, 0);
    }
  }
}

template <typename Traits>
void rcc_model<Traits>::oscillators_written(
    std::string_view reg,
    std::uint32_t    word,
    std::uint32_t    old,
    std::uint32_t    now)
{
  auto used = in_use();
  if constexpr(Traits::pll1_input_held_while_on)
  {
    auto const pll1 = std::to_underlying(source::pll1);
    auto const written =
        Traits::oscillators[pll1].reg == reg ? osc_[pll1].on.of(now) != 0 : is_on(source::pll1);
    if((written or (used >> pll1 & 1u) != 0) and pll_input(0) != source::pll1)
    {
      used |= std::uint32_t{1} << std::to_underlying(pll_input(0));
    }
  }
  for(auto const& [index, names] : std::views::enumerate(Traits::oscillators))
  {
    if(names.reg != reg)
    {
      continue;
    }
    auto const& osc = osc_[index];
    now             = osc.ready.with(now, osc.ready.of(old));
    if((used >> static_cast<std::uint32_t>(index) & 1u) != 0)
    {
      now = osc.on.with(now, 1);
    }
  }
  check_pll_enable(reg, word, old, now);
  regs_.poke(word, now);
  for(auto const& [index, names] : std::views::enumerate(Traits::oscillators))
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
      if(auto const at = static_cast<std::size_t>(index); at >= std::to_underlying(source::pll1))
      {
        load_fraction(at - std::to_underlying(source::pll1));
      }
    }
    else if(was and not is and not is_on(static_cast<source>(index)))
    {
      stop(static_cast<source>(index));
    }
  }
  settle(true);
}

template <typename Traits>
auto rcc_model<Traits>::worst_level() const -> std::optional<std::uint32_t>
{
  std::optional<std::uint32_t> level;
  for(auto* const peer : this->ctx_.peers.all())
  {
    if(auto const* const range = dynamic_cast<voltage_range_source const*>(peer))
    {
      level = std::max(range->active_level(), range->target_level());
    }
  }
  return level;
}

template <typename Traits>
auto rcc_model<Traits>::pll_violations(std::size_t pll, [[maybe_unused]] std::uint32_t level) const
    -> std::string
{
  if constexpr(requires { Traits::pll_limit_tables; })
  {
    auto const& fields = pll_[pll];
    auto const& node   = Traits::pll_nodes[pll];
    auto const  table  = fields.vcosel ? get(regs_, *fields.vcosel) : 0u;
    auto const  origin = std::to_underlying(pll_input(pll));
    if(level >= pll_limit_levels
       or table >= Traits::pll_limit_tables.size()
       or origin >= std::to_underlying(source::pll1)
       or Traits::oscillators[origin].rate == nullptr)
    {
      return {};
    }
    auto const&       limits = Traits::pll_limit_tables[table][level];
    rate_inputs const inputs{
        .regs = regs_, .names = names_, .options = options_, .hsi_divider = state_.hsi_divider};
    auto const input   = Traits::oscillators[origin].rate(inputs);
    auto const divisor = fields.m ? node.m_decode(get(regs_, *fields.m)) : 1;
    if(input == 0 or divisor == 0)
    {
      return {};
    }
    auto const fraction  = fields.fracen and fields.frac and get(regs_, *fields.fracen) != 0
                             ? get(regs_, *fields.frac)
                             : 0;
    auto const reference = input / divisor;
    auto const vco =
        input * (node.n_decode(get(regs_, fields.n)) * frac_one + fraction) / (divisor * frac_one);
    std::string text;
    auto const  add = [&](std::string const& what) { text += (text.empty() ? "" : ", ") + what; };
    if(limits.output_max_hz == 0)
    {
      add("a PLL is not allowed at this level");
    }
    if(reference < limits.input_min_hz or reference > limits.input_max_hz)
    {
      add(std::format(
          "reference clock {} Hz outside {} to {} Hz", reference, limits.input_min_hz,
          limits.input_max_hz));
    }
    if(vco < limits.vco_min_hz or vco > limits.vco_max_hz)
    {
      add(std::format("VCO {} Hz outside {} to {} Hz", vco, limits.vco_min_hz, limits.vco_max_hz));
    }
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      auto const& enable  = fields.output_enable[output];
      auto const  divider = node.output_decode[output](get(regs_, fields.divider[output]));
      if(limits.output_max_hz != 0
         and (not enable or get(regs_, *enable) != 0)
         and divider != 0
         and vco / divider > limits.output_max_hz)
      {
        add(std::format(
            "output {} at {} Hz above {} Hz", output, vco / divider, limits.output_max_hz));
      }
    }
    if(text.empty())
    {
      return text;
    }
    return std::format(
        "PLL{} is on at {}{}, outside its limits: {}", pll + 1, Traits::pll_level_label,
        level + Traits::pll_level_first, text);
  }
  return {};
}

template <typename Traits>
void rcc_model<Traits>::check_pll_enable(
    std::string_view reg,
    std::uint32_t    word,
    std::uint32_t    old,
    std::uint32_t    now)
{
  for(auto const& [index, names] : std::views::enumerate(Traits::oscillators))
  {
    auto const at = static_cast<std::size_t>(index);
    if(names.reg != reg
       or at < std::to_underlying(source::pll1)
       or osc_[at].on.of(old) != 0
       or osc_[at].on.of(now) == 0)
    {
      continue;
    }
    auto const level = worst_level();
    auto const pll   = at - std::to_underlying(source::pll1);
    auto       text  = level ? pll_violations(pll, *level) : std::string{};
    if(text.empty())
    {
      continue;
    }
    if(this->ctx_.strict_peripherals)
    {
      regs_.poke(word, old);
    }
    this->warn_once_or_refuse(pll_range_logged_[pll], reg, word, std::move(text));
  }
}

template <typename Traits> void rcc_model<Traits>::on_voltage_range_change()
{
  auto const level = worst_level();
  if(not level)
  {
    return;
  }
  static_cast<void>(voltage_range_refused(*level));
}

template <typename Traits>
auto rcc_model<Traits>::voltage_range_refused(std::uint32_t level) -> bool
{
  auto refused = false;
  for(std::size_t pll = 0; pll < pll_.size(); ++pll)
  {
    if(not is_on(static_cast<source>(std::to_underlying(source::pll1) + pll)))
    {
      continue;
    }
    if(auto text = pll_violations(pll, level); not text.empty())
    {
      refused = true;
      if(not std::exchange(pll_range_logged_[pll], true))
      {
        this->warn(
            Traits::oscillators[std::to_underlying(source::pll1) + pll].reg, std::move(text));
      }
    }
  }
  return refused;
}

template <typename Traits>
auto rcc_model<Traits>::held(std::uint32_t word, std::uint32_t old, std::uint32_t now) const
    -> std::uint32_t
{
  for(auto const& [bits, which] : held_)
  {
    if(bits.offset == word and osc_[std::to_underlying(which)].on.of(old) != 0)
    {
      now = bits.with(now, bits.of(old));
    }
  }
  return now;
}

template <typename Traits> void rcc_model<Traits>::cr_written(std::uint32_t old, std::uint32_t now)
{
  if(not is_ready(source::lse))
  {
    for(auto const& bits : lse_locked_)
    {
      now = bits.with(now, 0);
    }
  }
  now = held(control_.offset, old, now);
  if constexpr(Traits::css_held)
  {
    now |= old & names_[rcc_role::css_on].mask();
  }
  if(auto const select = names_.find(rcc_role::msi_select);
     select and select->offset == control_.offset)
  {
    now |= old & select->mask();
  }
  if constexpr(requires { Traits::writable_msi_ranges; })
  {
    if(auto const range = names_[rcc_role::msis_range];
       range.of(now) >= Traits::writable_msi_ranges)
    {
      now = range.with(now, range.of(old));
    }
  }
  if constexpr(Traits::hsi_divider)
  {
    now = divider_written(old, now);
  }
  oscillators_written(Traits::control_register, control_.offset, old, now);
}

template <typename Traits>
auto rcc_model<Traits>::divider_written(std::uint32_t old, std::uint32_t now) -> std::uint32_t
{
  auto const divider = names_[rcc_role::hsi_divider];
  auto const flag    = names_[rcc_role::hsi_divider_flag];
  if((in_use() >> std::to_underlying(source::pll1) & 1u) != 0
     and pll_input(0) == Traits::divided_oscillator)
  {
    now = divider.with(now, divider.of(old));
  }
  if(divider.of(now) != divider.of(old))
  {
    now = flag.with(now, flag.of(old));
    if constexpr(Traits::hsi_divider)
    {
      auto const timing = Traits::hsi_divider_timings[state_.hsi_divider][divider.of(now)];
      auto const old_hz = Traits::undivided_hsi_hz >> state_.hsi_divider;
      auto const cycles = [old_hz](std::uint64_t count) {
        return (count * ps_per_second + old_hz - 1) / old_hz;
      };
      state_.divider_set_ps   = ctx_.clock.time().ps + cycles(timing.set);
      state_.divider_clearing = 1;
      schedule(divider_kind, cycles(timing.clear));
    }
  }
  return now;
}

template <typename Traits>
void rcc_model<Traits>::icscr1_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(
      names_[rcc_role::msi_select].offset, now | (old & names_[rcc_role::msi_select].mask()));
  settle(true);
}

template <typename Traits> void rcc_model<Traits>::tree_written(std::uint32_t, std::uint32_t)
{
  settle(true);
}

template <typename Traits>
template <std::size_t Pll>
void rcc_model<Traits>::pll_config_written(std::uint32_t old, std::uint32_t now)
{
  auto const& fields = pll_[Pll];
  if(input_locked(Pll))
  {
    for(auto const& kept : {std::optional{fields.src}, fields.range, fields.m})
    {
      if(kept)
      {
        now = kept->with(now, kept->of(old));
      }
    }
  }
  if(locked(Pll))
  {
    if constexpr(Traits::pll_nodes[Pll].config == Traits::pll_nodes[Pll].dividers)
    {
      now = fields.n.with(now, fields.n.of(old));
      for(std::size_t output = 0; output < Traits::pll_nodes[Pll].outputs; ++output)
      {
        now = fields.divider[output].with(now, fields.divider[output].of(old));
      }
    }
    if constexpr(Traits::pll_nodes[Pll].outputs_held_while_on)
    {
      for(auto const& kept : fields.output_enable)
      {
        if(kept)
        {
          now = kept->with(now, kept->of(old));
        }
      }
    }
  }
  auto const drives_core = Pll == 0 and (in_use() >> std::to_underlying(source::pll1) & 1u) != 0;
  if(auto const& enable = fields.output_enable[system_output]; drives_core and enable)
  {
    now = enable->with(now, 1);
  }
  regs_.poke(fields.src.offset, now);
  if(fields.fracen and fields.fracen->of(old) == 0 and fields.fracen->of(now) != 0)
  {
    load_fraction(Pll);
  }
  settle(true);
}

template <typename Traits>
template <std::size_t Pll>
void rcc_model<Traits>::pll_dividers_written(std::uint32_t old, std::uint32_t now)
{
  auto const& fields = pll_[Pll];
  if(locked(Pll))
  {
    now = fields.n.with(now, fields.n.of(old));
    for(std::size_t output = 0; output < Traits::pll_nodes[Pll].outputs; ++output)
    {
      auto const& kept = fields.divider[output];
      now              = kept.with(now, kept.of(old));
    }
  }
  regs_.poke(fields.n.offset, now);
  settle(true);
}

template <typename Traits> void rcc_model<Traits>::refuse_stopped_clock()
{
  if(std::exchange(core_stop_logged_, true))
  {
    return;
  }
  warn(
      std::ranges::find(Traits::names, rcc_role::sw, &named_field<rcc_role>::role)->name.reg,
      std::format(
          "the selected system clock source runs at 0 Hz, the core clock stays at {} Hz",
          ctx_.clock.core_hz()));
}

template <typename Traits> auto rcc_model<Traits>::locked(std::size_t pll) const -> bool
{
  auto const which = static_cast<source>(std::to_underlying(source::pll1) + pll);
  return is_on(which) or is_ready(which);
}

template <typename Traits> auto rcc_model<Traits>::input_locked(std::size_t pll) const -> bool
{
  auto const& src = pll_[pll].src;
  return std::ranges::any_of(std::views::iota(std::size_t{0}, pll_.size()), [&](std::size_t other) {
    return pll_[other].src.offset == src.offset
       and pll_[other].src.mask() == src.mask()
       and locked(other);
  });
}

template <typename Traits>
void rcc_model<Traits>::bdcr_written(std::uint32_t old, std::uint32_t now)
{
  now = held(bdcr_.offset, old, now);
  if constexpr(Traits::backup_register == Traits::reset_flags_register)
  {
    now = flags_written(old, now);
  }
  if(names_[rcc_role::backup_reset].of(now) != 0)
  {
    now = names_[rcc_role::backup_reset].with(now & ~backup_reset_mask_, 1);
  }
  else if(names_[rcc_role::rtc_select].of(old) != 0)
  {
    now = names_[rcc_role::rtc_select].with(now, names_[rcc_role::rtc_select].of(old));
  }
  oscillators_written(Traits::backup_register, bdcr_.offset, old, now);
  if(names_[rcc_role::backup_reset].of(old) == 0 and names_[rcc_role::backup_reset].of(now) != 0)
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

template <typename Traits> void rcc_model<Traits>::csr_written(std::uint32_t old, std::uint32_t now)
{
  now = flags_written(old, now);
  if constexpr(
      std::ranges::contains(
          Traits::oscillators, Traits::reset_flags_register, &oscillator_names<signal>::reg))
  {
    oscillators_written(Traits::reset_flags_register, csr_.offset, old, now);
  }
  else
  {
    regs_.poke(csr_.offset, now);
  }
}

template <typename Traits>
auto rcc_model<Traits>::flags_written(std::uint32_t old, std::uint32_t now) const -> std::uint32_t
{
  if constexpr(Traits::msi_standby_ranges)
  {
    if(read(rcc_role::msi_select) == 0)
    {
      for(auto const role : {rcc_role::msis_standby_range, rcc_role::msik_standby_range})
      {
        if(auto const range = names_.find(role))
        {
          now = range->with(now, range->of(old));
        }
      }
    }
  }
  if(names_[rcc_role::remove_flags].of(now) != 0)
  {
    for(auto const& flag : flags_)
    {
      now = flag.with(now, 0);
    }
  }
  return names_[rcc_role::remove_flags].with(now, 0);
}

template <typename Traits>
void rcc_model<Traits>::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == event_count<Traits> + 1)
  {
    auto const word = ctx_.services.read(wwdg_option_->at, 4, initiator::debugger);
    if(word and (*word & wwdg_option_->mask) == 0)
    {
      auto const before = regs_.peek(wwdg_enable_.offset);
      update(regs_, wwdg_enable_, 1);
      if(not Traits::wwdg_sleep.reg.empty())
      {
        update(regs_, wwdg_sleep_, 1);
      }
      settle(true);
      enables_written(wwdg_enable_.offset, before, regs_.peek(wwdg_enable_.offset));
    }
    return;
  }
  if(kind == event_count<Traits>)
  {
    auto const found = std::ranges::find(state_.notes, payload, &output_note::id);
    if(found == state_.notes.end())
    {
      return;
    }
    auto const note = *found;
    state_.notes.erase(found);
    std::vector<std::uint64_t> ids;
    for(auto const& entry : output_watchers_->entries)
    {
      if(entry.output == note.output and entry.id < note.upto)
      {
        ids.push_back(entry.id);
      }
    }
    for(auto const id : ids)
    {
      auto const entry = std::ranges::find(output_watchers_->entries, id, &output_watch::id);
      if(entry != output_watchers_->entries.end())
      {
        auto const call = entry->call;
        (*call)(note.hz, vtime{note.at_ps});
      }
    }
    return;
  }
  if(kind >= event_count<Traits>
     or payload != state_.generation[kind]
     or (state_.pending >> kind & 1u) == 0)
  {
    return;
  }
  state_.pending &= ~(std::uint32_t{1} << kind);
  if(kind >= first_dependent_event<Traits>)
  {
    if constexpr(dependent_count<Traits> != 0)
    {
      update(regs_, dependents_[kind - first_dependent_event<Traits>].ready, 1);
    }
  }
  else if(kind == divider_kind)
  {
    if(std::exchange(state_.divider_clearing, 0) != 0)
    {
      update(regs_, names_[rcc_role::hsi_divider_flag], 0);
      schedule(divider_kind, state_.divider_set_ps - ctx_.clock.time().ps);
      return;
    }
    update(regs_, names_[rcc_role::hsi_divider_flag], 1);
    state_.hsi_divider = read(rcc_role::hsi_divider);
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

template <typename Traits> void rcc_model<Traits>::follow_dependents()
{
  if constexpr(dependent_count<Traits> != 0)
  {
    rate_inputs const inputs{
        .regs = regs_, .names = names_, .options = options_, .hsi_divider = state_.hsi_divider};
    for(auto const& [index, names] : std::views::enumerate(Traits::dependent_clocks))
    {
      auto const& fields  = dependents_[index];
      auto const  kind    = first_dependent_event<Traits> + static_cast<std::size_t>(index);
      auto const  pending = (state_.pending >> kind & 1u) != 0;
      if(get(regs_, fields.enable) == 0 or not is_ready(names.from))
      {
        update(regs_, fields.ready, 0);
        if(pending)
        {
          cancel(kind);
        }
      }
      else if(get(regs_, fields.ready) == 0 and not pending)
      {
        auto const hz = std::max<std::uint64_t>(
            Traits::oscillators[std::to_underlying(names.from)].rate(inputs), 1);
        schedule(kind, std::uint64_t{names.cycles} * ps_per_second / hz);
      }
    }
  }
}

template <typename Traits> void rcc_model<Traits>::settle(bool notify)
{
  follow_dependents();
  for(std::size_t pll = 0; pll < pll_.size(); ++pll)
  {
    auto const  which = static_cast<source>(std::to_underlying(source::pll1) + pll);
    auto const  index = std::to_underlying(which);
    auto const  input = pll_input(pll);
    auto const& m     = pll_[pll].m;
    auto const  fed   = input != source::pll1
                  and is_ready(input)
                  and (not m or Traits::pll_nodes[pll].m_decode(get(regs_, *m)) != 0);
    if(is_ready(which) and not fed)
    {
      stop(which);
    }
    else if(is_on(which) and not is_ready(which) and fed and (state_.pending >> index & 1u) == 0)
    {
      schedule(index);
    }
  }
  auto const target = read(rcc_role::sw) & Traits::sw_mask;
  if(auto const input = system_source(target);
     input and is_ready(*input) and target != (read(rcc_role::sws) & Traits::sw_mask))
  {
    update(regs_, names_[rcc_role::sws], target);
  }
  publish(notify);
}

template <typename Traits>
auto rcc_model<Traits>::rates() const -> std::array<std::uint64_t, signal_count>
{
  std::array<std::uint64_t, signal_count> hz{};
  auto const at    = [&](signal which) -> std::uint64_t& { return hz[std::to_underlying(which)]; };
  auto const ready = [&](source which) { return is_ready(which); };
  rate_inputs const inputs{
      .regs = regs_, .names = names_, .options = options_, .hsi_divider = state_.hsi_divider};
  for(auto const& [index, names] : std::views::enumerate(Traits::oscillators))
  {
    if(names.rate != nullptr)
    {
      at(names.out) = ready(static_cast<source>(index)) ? names.rate(inputs) : 0;
    }
  }
  if constexpr(requires { Traits::dependent_clocks; })
  {
    for(auto const& [names, fields] : std::views::zip(Traits::dependent_clocks, dependents_))
    {
      at(names.out) = get(regs_, fields.ready) != 0 ? at(names.in) : 0;
    }
  }
  for(auto const& node : Traits::divided)
  {
    at(node.out) = at(node.in) / node.divider;
  }
  for(std::size_t pll = 0; pll < pll_.size(); ++pll)
  {
    auto const& fields  = pll_[pll];
    auto const& node    = Traits::pll_nodes[pll];
    auto const  which   = static_cast<source>(std::to_underlying(source::pll1) + pll);
    auto const  input   = at(Traits::pll_sources[get(regs_, fields.src)]);
    auto const  frac    = state_.fraction[pll];
    auto const  divisor = fields.m ? node.m_decode(get(regs_, *fields.m)) : 1;
    auto const  vco =
        ready(which) and divisor != 0
            ? input * (node.n_decode(get(regs_, fields.n)) * frac_one + frac) / (divisor * frac_one)
            : 0;
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      auto const& enable  = fields.output_enable[output];
      auto const  divider = node.output_decode[output](get(regs_, fields.divider[output]));
      at(node.out[output]) =
          (not enable or get(regs_, *enable) != 0) and divider != 0 ? vco / divider : 0;
    }
  }
  at(signal::sysclk) = at(Traits::system_sources[read(rcc_role::sws) & Traits::sw_mask]);
  at(signal::hclk)   = at(signal::sysclk) / hpre_divider(read(rcc_role::hpre));
  for(auto const& bus : Traits::buses)
  {
    if(auto const prescaler = names_.find(bus.prescaler))
    {
      at(bus.out) = at(signal::hclk) / ppre_divider(get(regs_, *prescaler));
    }
  }
  for(auto const& [node, select] : std::views::zip(Traits::signal_muxes, selects_))
  {
    auto const code = get(regs_, select);
    at(node.out)    = code < node.inputs.size() ? at(node.inputs[code]) : 0;
  }
  auto const& rtc_from = Traits::rtc_sources[read(rcc_role::rtc_select)];
  auto const  divider  = rtc_from.divider(inputs);
  at(signal::rtc) =
      read(rcc_role::rtc_enable) != 0 and divider != 0 ? at(rtc_from.from) / divider : 0;
  return hz;
}

template <typename Traits>
void rcc_model<Traits>::follow_outputs(
    std::array<std::uint64_t, signal_count> const& hz,
    bool                                           notify)
{
  if constexpr(output_count<Traits> != 0)
  {
    for(auto const& [index, names] : std::views::enumerate(Traits::clock_outputs))
    {
      auto const& fields = outputs_[index];
      auto const  from   = names.inputs[get(regs_, fields.select) & 1u];
      auto const  rate   = get(regs_, fields.enable) != 0 ? hz[std::to_underlying(from)] : 0;
      if(std::exchange(output_hz_[index], rate) == rate or not notify)
      {
        continue;
      }
      post_output(static_cast<std::size_t>(index), rate);
    }
  }
}

template <typename Traits>
void rcc_model<Traits>::post_output(std::size_t index, std::uint64_t rate)
{
  if(std::ranges::none_of(output_watchers_->entries, [&](output_watch const& entry) {
       return entry.output == index;
     }))
  {
    return;
  }
  auto const at = ctx_.clock.time().ps;
  auto const id = state_.next_note++;
  state_.notes.push_back(
      {.id     = id,
       .upto   = output_watchers_->next,
       .at_ps  = at,
       .hz     = rate,
       .output = static_cast<std::uint8_t>(index)});
  ctx_.events.schedule_at(vtime{at}, event_count<Traits>, id, core::host_lane, ctx_.clock);
}

template <typename Traits>
auto rcc_model<Traits>::output_index(std::string_view port, std::uint8_t pin) const -> std::size_t
{
  if constexpr(output_count<Traits> != 0)
  {
    for(auto const& [index, names] : std::views::enumerate(Traits::clock_outputs))
    {
      if(names.port == port and names.pin == pin)
      {
        return static_cast<std::size_t>(index);
      }
    }
  }
  return output_count<Traits>;
}

template <typename Traits>
auto rcc_model<Traits>::clock_hz(std::string_view port, std::uint8_t pin) const -> std::uint64_t
{
  auto const index = output_index(port, pin);
  return index < output_count<Traits> ? output_hz_[index] : 0;
}

template <typename Traits>
auto rcc_model<Traits>::watch_clock(
    std::string_view                                          port,
    std::uint8_t                                              pin,
    std::move_only_function<void(std::uint64_t hz, vtime at)> watcher) -> subscription
{
  auto const index = output_index(port, pin);
  if(index >= output_count<Traits>)
  {
    return subscription{};
  }
  auto const id = output_watchers_->next++;
  output_watchers_->entries.push_back(
      output_watch{
          .id     = id,
          .output = index,
          .call   = std::make_shared<std::move_only_function<void(std::uint64_t, vtime)>>(
              std::move(watcher))});
  return subscription{[table = std::weak_ptr<output_table>{output_watchers_}, id] {
    if(auto const live = table.lock())
    {
      std::erase_if(live->entries, [id](auto const& entry) { return entry.id == id; });
    }
  }};
}

template <typename Traits> void rcc_model<Traits>::publish(bool notify)
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
    auto const from    = code < entry.inputs->size() ? (*entry.inputs)[code] : signal::none;
    auto const enabled = get(regs_, entry.enable) != 0 ? std::uint8_t{1} : std::uint8_t{0};
    set(entry.domain, enabled != 0 ? hz[std::to_underlying(from)] : 0);
    changed   = changed or (known and published != enabled);
    published = enabled;
  }
  rate_inputs const inputs{
      .regs = regs_, .names = names_, .options = options_, .hsi_divider = state_.hsi_divider};
  for(auto const& entry : timers_)
  {
    auto const bus      = hz[std::to_underlying(entry.bus)];
    auto const multiple = Traits::timer_multiplier(inputs, entry.prescaler);
    set(entry.domain, get(regs_, entry.enable) != 0 ? bus * multiple : 0);
  }
  for(auto const& entry : bus_gates_)
  {
    set(entry.domain, get(regs_, entry.enable) != 0 ? hz[std::to_underlying(entry.bus)] : 0);
  }
  if(not timic_taps_.empty())
  {
    auto const value = get(regs_, timic_);
    auto const open  = ((value >> (timic_.width - 1)) & 1u) != 0;
    for(auto const& tap : timic_taps_)
    {
      auto const from =
          tap.select_bit >= 0 and ((value >> tap.select_bit) & 1u) != 0 ? tap.set : tap.clear;
      set(tap.domain, open ? hz[std::to_underlying(from)] : 0);
    }
  }
  follow_outputs(hz, notify);
  auto const core = hz[std::to_underlying(signal::hclk)];
  if(not options_.fixed_core_clock and core == 0)
  {
    refuse_stopped_clock();
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

template <typename Traits> void rcc_model<Traits>::reset(register_file& regs, reset_kind kind)
{
  auto const before_hz  = output_hz_;
  auto const power      = std::ranges::contains(Traits::power_resets, kind);
  auto const kept_bdcr  = regs.peek(bdcr_.offset) & backup_mask_;
  auto       kept_words = kept_
                        | std::views::transform([&](auto const& bits) {
                      return std::pair{bits.first, regs.peek(bits.first) & bits.second};
                          })
                        | std::ranges::to<std::vector>();
  auto       kept_flags = std::uint32_t{};
  for(auto const& flag : flags_)
  {
    kept_flags |= regs.peek(csr_.offset) & flag.mask();
  }
  for(auto const& name : this->claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto csr = describe(Traits::reset_flags_register).reset;
  if(not power)
  {
    for(auto const& flag : flags_)
    {
      csr = flag.with(csr, 0);
    }
    csr |= kept_flags;
  }
  for(auto const name : reset_flags_in(Traits::reset_flag_rows, kind))
  {
    if(not name.empty())
    {
      csr = field(Traits::reset_flags_register, name).with(csr, 1);
    }
  }
  regs.poke(csr_.offset, csr);
  if(not power)
  {
    regs.poke(bdcr_.offset, (regs.peek(bdcr_.offset) & ~backup_mask_) | kept_bdcr);
    for(auto const& [kept, value] : std::views::zip(kept_, kept_words))
    {
      regs.poke(kept.first, (regs.peek(kept.first) & ~kept.second) | value.second);
    }
  }
  for(auto& generation : state_.generation)
  {
    ++generation;
  }
  state_.pending = 0;
  state_.notes.clear();
  state_.lsi_forced = 0;
  state_.fraction   = {};
  if constexpr(Traits::hsi_divider)
  {
    state_.divider_clearing = 0;
    state_.hsi_divider      = read(rcc_role::hsi_divider);
  }
  if(not is_on(source::lsi))
  {
    update(regs_, osc_[std::to_underlying(source::lsi)].ready, 0);
  }
  for(std::size_t index = 0; index < std::to_underlying(source::pll1); ++index)
  {
    auto const which = static_cast<source>(index);
    if(is_on(which) and not is_ready(which))
    {
      start(which);
    }
  }
  follow_dependents();
  publish(false);
  for(std::size_t index = 0; index < output_count<Traits>; ++index)
  {
    if(output_hz_[index] != before_hz[index])
    {
      post_output(index, output_hz_[index]);
    }
  }
  drive_enables(false);
  if(wwdg_option_)
  {
    ctx_.events.schedule_at(ctx_.clock.time(), event_count<Traits> + 1, 0, ctx_.clock);
  }
}

template <typename Traits> void rcc_model<Traits>::cicr_written(std::uint32_t, std::uint32_t now)
{
  apply_crosses(flag_clears_, now);
  regs_.poke(reg("RCC_CICR").offset, 0);
}

template <typename Traits> void rcc_model<Traits>::add_security_rules()
{
  flag_clears_          = crosses_from("RCC_CICR");
  privilege_            = reg("RCC_PRIVCFGR");
  secure_privileged_    = field("RCC_PRIVCFGR", "SPRIV");
  nonsecure_privileged_ = field("RCC_PRIVCFGR", "NSPRIV");
  for(auto const& group : Traits::secured)
  {
    for(auto const name : std::views::split(group.fields, ' '))
    {
      secured_.emplace_back(
          field("RCC_SECCFGR", group.bit), field(group.reg, std::string_view{name}));
    }
  }
  for(auto const& group : Traits::backup_secured)
  {
    for(auto const name : std::views::split(group.fields, ' '))
    {
      auto const bits = field(group.reg, std::string_view{name});
      backup_secured_.emplace_back(bits.offset, bits.mask());
    }
  }
  for(auto const name : Traits::unit_registers)
  {
    for(auto const& bits : describe(name).fields)
    {
      auto const suffix = std::ranges::find_if(
          Traits::unit_suffixes, [&](std::string_view s) { return bits.name.ends_with(s); });
      if(suffix == Traits::unit_suffixes.end())
      {
        continue;
      }
      auto const stem = std::string_view{bits.name}.substr(0, bits.name.size() - suffix->size());
      auto owners     = Traits::unit_aliases
                      | std::views::filter([stem](unit_alias const& a) { return a.first == stem; })
                      | std::views::values
                      | std::ranges::to<std::vector>();
      if(owners.empty())
      {
        owners.push_back(stem);
      }
      for(auto const owner : owners)
      {
        if(auto const unit = unit_named(ctx_.part, owner))
        {
          units_.push_back(
              {.offset = reg(name).offset, .bits = field(name, bits.name).mask(), .unit = *unit});
        }
      }
    }
  }
  for(auto const& name : this->claims())
  {
    std::uint32_t mask = 0;
    for(auto const& bits : describe(name).fields)
    {
      mask |= field(name, bits.name).mask();
    }
    words_.emplace_back(reg(name).offset, mask);
  }
}

template <typename Traits> void rcc_model<Traits>::resolve_units()
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

template <typename Traits>
auto rcc_model<Traits>::unit_is_secure(unit_bits const& rule) const -> bool
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

template <typename Traits> auto rcc_model<Traits>::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone and secure_ ? regs_.peek(secure_->offset) : 0;
}

template <typename Traits> auto rcc_model<Traits>::secure_function() const -> bool
{
  return secure_word() != 0;
}

template <typename Traits>
auto rcc_model<Traits>::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  auto const all = ~std::uint32_t{};
  if(auto const head = config_word_guard(
         secure_, privilege_, secure_privileged_, regs_, ctx_.trustzone, word, ctx, write, [&] {
           if(unit_)
           {
             illegal_.raise(ctx_.peers, *unit_, ctx.by);
           }
         }))
  {
    return *head;
  }
  auto const privilege = regs_.peek(privilege_.offset);
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
  if(std::ranges::contains(backup_secured_, word, &std::pair<std::uint32_t, std::uint32_t>::first)
     and std::ranges::any_of(backup_units_, [](trustzone_aware const* unit) {
           return unit != nullptr and unit->secure_function();
         }))
  {
    for(auto const& [offset, bits] : backup_secured_)
    {
      if(offset == word)
      {
        secured |= bits;
      }
    }
  }
  auto const known =
      std::ranges::find(words_, word, &std::pair<std::uint32_t, std::uint32_t>::first);
  auto const whole = known != words_.end() ? known->second : all;
  return guard_of(secured, true, secure_privileged_.of(privilege) != 0, ctx)
       | guard_of(whole & ~secured, false, nonsecure_privileged_.of(privilege) != 0, ctx);
}

template <typename Traits> void rcc_model<Traits>::force_lsi()
{
  auto const lsi    = source::lsi;
  auto const was    = is_on(lsi);
  state_.lsi_forced = 1;
  if(not was and not is_ready(lsi))
  {
    start(lsi);
  }
}

template <typename Traits> void rcc_model<Traits>::load(std::span<std::byte const> in)
{
  base::load(in);
  for(auto& note : state_.notes)
  {
    note.upto = std::min(note.upto, output_watchers_->next);
  }
  follow_outputs(rates(), false);
  kernel_enables_.clear();
  for(auto const& entry : kernels_)
  {
    kernel_enables_.push_back(get(regs_, entry.enable) != 0 ? 1 : 0);
  }
  drive_enables(false);
}

template <typename Traits>
auto rcc_model<Traits>::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const                     word   = offset & ~3u;
  auto const                     before = regs.peek(word);
  std::expected<void, bus_error> done{};
  try
  {
    done = base::on_write(regs, offset, size, old, value, ctx);
  }
  catch(...)
  {
    enables_written(word, before, regs.peek(word));
    throw;
  }
  enables_written(word, before, regs.peek(word));
  return done;
}

template <typename Traits>
void rcc_model<Traits>::enables_written(std::uint32_t word, std::uint32_t old, std::uint32_t now)
{
  if(old == now)
  {
    return;
  }
  std::exception_ptr first;
  for(auto const& entry : enables_)
  {
    if(entry.bit.offset != word or entry.bit.of(old) == entry.bit.of(now))
    {
      continue;
    }
    try
    {
      ctx_.services.gate_bus_clock(entry.instance, entry.bit.of(now) == 0);
    }
    catch(...)
    {
      if(not first)
      {
        first = std::current_exception();
      }
    }
  }
  if(first)
  {
    std::rethrow_exception(first);
  }
}

template <typename Traits> void rcc_model<Traits>::drive_enables(bool from_reset_values)
{
  for(auto const& entry : enables_)
  {
    auto const word = from_reset_values ? describe(entry.reg).reset : regs_.peek(entry.bit.offset);
    ctx_.services.gate_bus_clock(entry.instance, entry.bit.of(word) == 0);
  }
}

template <typename Traits>
auto rcc_model<Traits>::bus_enabled(core::clock_domain kernel) const -> bool
{
  auto const found = std::ranges::find(kernels_, kernel, &stm32::kernel<signal>::domain);
  return found == kernels_.end() or get(regs_, found->enable) != 0;
}

template <typename Traits>
template <std::size_t Reg>
void rcc_model<Traits>::resets_written(std::uint32_t old, std::uint32_t now)
{
  std::exception_ptr first;
  for(auto const& entry : resets_)
  {
    if(entry.reg != Reg or entry.bit.of(old) == entry.bit.of(now))
    {
      continue;
    }
    try
    {
      ctx_.services.hold_in_reset(entry.instance, entry.bit.of(now) != 0);
    }
    catch(...)
    {
      if(not first)
      {
        first = std::current_exception();
      }
    }
  }
  if(first)
  {
    std::rethrow_exception(first);
  }
}

template <typename Traits> void rcc_model<Traits>::hold_hsi16(bool held)
{
  static_cast<void>(held);
}
}

auto rcc_factory(rcc_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rcc_model<u5_rcc>>(ctx, desc, options);
  };
}

auto rcc_h5_factory(rcc_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rcc_model<h5_rcc>>(ctx, desc, options);
  };
}

auto rcc_l1_factory(rcc_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rcc_model<l1_rcc>>(ctx, desc, options);
  };
}

auto rcc_l4_factory(rcc_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<rcc_model<l4_rcc>>(ctx, desc, options);
  };
}

namespace
{
constexpr auto rcc_table = std::to_array<builtin>({
    row_of<^^rcc_factory, ^^void, ^^board::rcc>(
        {.ip = "RCC", .ip_version_glob = "STM32U5_rcc_*", .families = family::u5}),
    row_of<^^rcc_h5_factory, ^^void, ^^board::rcc>(
        {.ip = "RCC", .ip_version_glob = "STM32H5_rcc_*", .families = family::h5}),
    row_of<^^rcc_l1_factory, ^^void, ^^board::rcc>(
        {.ip = "RCC", .ip_version_glob = "STM32L1_rcc_*", .families = family::l1}),
    row_of<^^rcc_l4_factory, ^^void, ^^board::rcc>(
        {.ip = "RCC", .ip_version_glob = "STM32L4_rcc_*", .families = family::l4}),
});
}

auto rcc_rows() -> std::span<builtin const>
{
  return rcc_table;
}
}
