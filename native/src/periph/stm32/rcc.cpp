#include "backup_domain.hpp"
#include "factories.hpp"
#include "family_names.hpp"
#include "gtzc.hpp"
#include "hsi16_source.hpp"
#include "kernel_gate.hpp"
#include "lsi_source.hpp"
#include "model_base.hpp"
#include "rcc_h5.hpp"
#include "rcc_resets.hpp"
#include "rcc_u5.hpp"

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
auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

constexpr auto rcc_unit_suffixes =
    std::to_array<std::string_view>({"SMEN", "AMEN", "RST", "EN", "SEL"});

template <typename Traits>
inline constexpr std::size_t source_count = role_count<typename Traits::source>;

template <typename Traits>
inline constexpr std::size_t signal_count = role_count<typename Traits::signal>;

template <typename Traits>
inline constexpr std::size_t event_count = source_count<Traits> + (Traits::hsi_divider ? 2 : 1);

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

template <typename Traits> struct rcc_state
{
  std::array<std::uint32_t, event_count<Traits>> generation{};
  std::uint32_t                                  pending{};
  std::uint8_t                                   lsi_forced{};
  std::uint8_t                                   hsi16_held{};

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
  field_ref                                             src;
  std::optional<field_ref>                              range;
  std::optional<field_ref>                              m;
  std::optional<field_ref>                              fracen;
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

template <typename Signal> struct timer
{
  core::clock_domain domain;
  Signal             bus;
  rcc_role           prescaler;
  field_ref          enable;
};

/**
 * @brief RCC of the STM32U5, RM0456 section 11, and of the STM32H5, RM0481 section 11. Ready
 *        flags follow their enables after the
 *        board's ready delay. RCC writes RCC_CIFR storage directly when the matching RCC_CIER bit
 *        is set and raises no ready interrupt: firmware waiting on the RCC IRQ hangs, and the
 *        spin stop names its loop. A write that would stop the core clock, switching off the
 *        oscillator that drives it or changing PLL1 while it drives it, is refused and logs one
 *        warning per machine life. A started IWDG keeps the LSI on until the next reset, RM0456
 *        section 11.4.16. FLASH programming or erase keeps HSI16 on while it runs, RM0456
 *        sections 7.3.6 and 7.3.7. A change of an instance's bit in an RCC_xxxRSTR register,
 *        RM0456 11.8.21 to 11.8.28, holds or releases the instance through
 *        `core::machine_services::hold_in_reset()`, which resets its storage and calls its
 *        model's `periph::bus_reset_listener::on_bus_reset()`. The bit is `<instance>RST`, or
 *        the name of `reset_bit_names`. The STM32H5 reset registers are RM0481 11.8.19 to
 *        11.8.25, its reset flags `RCC_RSR`, RM0481 11.8.46.
 *
 *        STM32H5, RM0481 11.8.1: a write of `HSIDIV` sets the HSI rate at once, clears
 *        `HSIDIVF` and sets it again after the ready delay. Deviation: on silicon the rate
 *        follows when `HSIDIVF` sets. `HSIDIV` keeps its value while HSI feeds a PLL whose
 *        PLLxON is set. `TIMPRE`, RM0481 11.8.5, sets the timer clocks of APB1 and APB2.
 *        `VSWRST` resets the backup domain, RM0481 11.3.3. The STM32H5 traits hold no
 *        TrustZone tables.
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
 * @tparam Traits Family clock tree, u5_rcc or h5_rcc: the sources and signals, the oscillator,
 *         mux, timer and enable tables, the source maps of the PLLs, SYSCLK and RTC, the names
 *         of the single fields, and the TrustZone tables.
 */
template <typename Traits>
class rcc_model final : public dispatch_model<rcc_model<Traits>, rcc_state<Traits>>,
                        public lsi_source,
                        public hsi16_source,
                        public kernel_gate,
                        public trustzone_aware
{
  using base   = dispatch_model<rcc_model<Traits>, rcc_state<Traits>>;
  using source = typename Traits::source;
  using signal = typename Traits::signal;

  static constexpr std::size_t   source_count = stm32::source_count<Traits>;
  static constexpr std::size_t   signal_count = stm32::signal_count<Traits>;
  static constexpr std::uint32_t switch_kind  = source_count;
  static constexpr std::uint32_t divider_kind = source_count + 1;

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
  void               hold_hsi16(bool held) override;
  [[nodiscard]] auto bus_enabled(core::clock_domain kernel) const -> bool override;

  void               cr_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto divider_written(std::uint32_t old, std::uint32_t now) -> std::uint32_t;
  void               icscr1_written(std::uint32_t old, std::uint32_t now);
  void               tree_written(std::uint32_t old, std::uint32_t now);
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
  [[nodiscard]] auto system_source(std::uint32_t code) const -> std::optional<source>;
  [[nodiscard]] auto in_use() const -> std::uint32_t;
  [[nodiscard]] auto locked(std::size_t pll) const -> bool;
  [[nodiscard]] auto rates() const -> std::array<std::uint64_t, signal_count>;
  [[nodiscard]] auto enable_field(std::string_view name) const -> field_ref;
  [[nodiscard]] auto read(rcc_role role) const -> std::uint32_t;

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
  std::array<pll_fields, Traits::pll_nodes.size()>     pll_{};
  field_table<rcc_role>                                names_;
  std::array<field_ref, Traits::signal_muxes.size()>   selects_{};
  std::array<field_ref, Traits::reset_flags.size()>    flags_{};
  reg_ref                                              bdcr_;
  reg_ref                                              csr_;
  std::vector<std::pair<core::clock_domain, signal>>   signals_;
  std::vector<kernel<signal>>                          kernels_;
  std::vector<reset_entry>                             resets_;
  std::vector<std::string>                             reset_registers_;
  std::vector<std::uint8_t>                            kernel_enables_;
  std::vector<timer<signal>>                           timers_;
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

template <typename Traits> struct rcc_tables;

template <> struct rcc_tables<u5_rcc>
{
  using model = rcc_model<u5_rcc>;

  static constexpr std::array<write_entry<model>, 27> writes{
      {
       {"RCC_CR", &model::cr_written},
       {"RCC_ICSCR1", &model::icscr1_written},
       {"RCC_CFGR1", &model::tree_written},
       {"RCC_CFGR2", &model::tree_written},
       {"RCC_CFGR3", &model::tree_written},
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
      {"RCC_CIER",       nullptr             },
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

  static constexpr std::array<write_entry<model>, 26> writes{
      {
       {"RCC_CR", &model::cr_written},
       {"RCC_CFGR1", &model::tree_written},
       {"RCC_CFGR2", &model::tree_written},
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
      bdcr_{reg("RCC_BDCR")}, csr_{reg(Traits::reset_flags_register)},
      unit_{unit_named(ctx.part, desc.name)}
{
  for(auto const& [index, entry] : std::views::enumerate(Traits::signal_muxes))
  {
    selects_[index] = field(entry.reg, entry.field);
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
        .flag   = field("RCC_CIFR", names.flag),
        .enable = field("RCC_CIER", names.enable)};
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
    fields.n      = field(node.dividers, node.n);
    fields.frac   = named(node.fraction, node.frac);
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      fields.divider[output] = field(node.dividers, node.divider[output]);
    }
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      fields.output_enable[output] = named(node.config, node.output_enable[output]);
    }
  }
  for(auto const& [index, name] : std::views::enumerate(Traits::reset_flags))
  {
    flags_[index] = field(Traits::reset_flags_register, name);
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
      or (which == source::lsi and state_.lsi_forced != 0)
      or (which == Traits::programming_clock and state_.hsi16_held != 0);
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

template <typename Traits> void rcc_model<Traits>::schedule(std::uint32_t kind)
{
  auto const generation = ++state_.generation[kind];
  state_.pending |= std::uint32_t{1} << kind;
  auto const delay = static_cast<std::uint64_t>(options_.ready_delay.count()) * 1000u;
  ctx_.events.schedule_at(vtime{ctx_.clock.time().ps + delay}, kind, generation, ctx_.clock);
}

template <typename Traits> void rcc_model<Traits>::cancel(std::uint32_t kind)
{
  ++state_.generation[kind];
  state_.pending &= ~(std::uint32_t{1} << kind);
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
}

template <typename Traits>
void rcc_model<Traits>::oscillators_written(
    std::string_view reg,
    std::uint32_t    word,
    std::uint32_t    old,
    std::uint32_t    now)
{
  auto const used = in_use();
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
      if(osc.on.of(now) == 0)
      {
        refuse_core_stop(reg, std::format("{} = 0", names.on));
      }
      now = osc.on.with(now, 1);
    }
  }
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
    }
    else if(was and not is and not is_on(static_cast<source>(index)))
    {
      stop(static_cast<source>(index));
    }
  }
  settle(true);
}

template <typename Traits> void rcc_model<Traits>::cr_written(std::uint32_t old, std::uint32_t now)
{
  now |= old & names_[rcc_role::css_on].mask();
  if constexpr(Traits::hsi_divider)
  {
    now = divider_written(old, now);
  }
  oscillators_written("RCC_CR", names_[rcc_role::css_on].offset, old, now);
}

template <typename Traits>
auto rcc_model<Traits>::divider_written(std::uint32_t old, std::uint32_t now) -> std::uint32_t
{
  auto const divider = names_[rcc_role::hsi_divider];
  auto const flag    = names_[rcc_role::hsi_divider_flag];
  for(std::size_t pll = 0; pll < pll_.size(); ++pll)
  {
    auto const which = static_cast<source>(std::to_underlying(source::pll1) + pll);
    if(pll_input(pll) == Traits::divided_oscillator and is_on(which))
    {
      now = divider.with(now, divider.of(old));
    }
  }
  if(divider.of(now) != divider.of(old))
  {
    now = flag.with(now, 0);
    schedule(divider_kind);
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
  auto const& fields    = pll_[Pll];
  auto const  attempted = now;
  if(locked(Pll))
  {
    for(auto const& kept : {std::optional{fields.src}, fields.range, fields.m})
    {
      if(kept)
      {
        now = kept->with(now, kept->of(old));
      }
    }
  }
  auto const drives_core = Pll == 0 and (in_use() >> std::to_underlying(source::pll1) & 1u) != 0;
  if(drives_core)
  {
    if(auto const& enable = fields.output_enable[system_output])
    {
      now = enable->with(now, 1);
    }
    if(now != attempted)
    {
      refuse_core_stop(Traits::pll_nodes.front().config, "a PLL1 configuration change");
    }
  }
  regs_.poke(fields.src.offset, now);
  settle(true);
}

template <typename Traits>
template <std::size_t Pll>
void rcc_model<Traits>::pll_dividers_written(std::uint32_t old, std::uint32_t now)
{
  auto const& fields    = pll_[Pll];
  auto const  attempted = now;
  if(locked(Pll))
  {
    now = fields.n.with(now, fields.n.of(old));
    for(std::size_t output = 0; output < Traits::pll_nodes[Pll].outputs; ++output)
    {
      auto const& kept = fields.divider[output];
      now              = kept.with(now, kept.of(old));
    }
  }
  if(Pll == 0 and now != attempted and (in_use() >> std::to_underlying(source::pll1) & 1u) != 0)
  {
    refuse_core_stop(Traits::pll_nodes.front().dividers, "a PLL1 divider change");
  }
  regs_.poke(fields.n.offset, now);
  settle(true);
}

template <typename Traits>
void rcc_model<Traits>::refuse_core_stop(std::string_view reg, std::string_view what)
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

template <typename Traits> auto rcc_model<Traits>::locked(std::size_t pll) const -> bool
{
  auto const which = static_cast<source>(std::to_underlying(source::pll1) + pll);
  return is_on(which) or is_ready(which);
}

template <typename Traits>
void rcc_model<Traits>::bdcr_written(std::uint32_t old, std::uint32_t now)
{
  if(names_[rcc_role::backup_reset].of(now) != 0)
  {
    now = names_[rcc_role::backup_reset].with(0, 1);
  }
  else if(names_[rcc_role::rtc_select].of(old) != 0)
  {
    now = names_[rcc_role::rtc_select].with(now, names_[rcc_role::rtc_select].of(old));
  }
  oscillators_written("RCC_BDCR", bdcr_.offset, old, now);
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
  if constexpr(Traits::msi)
  {
    if(read(rcc_role::msi_select) == 0)
    {
      for(auto const role : {rcc_role::msis_standby_range, rcc_role::msik_standby_range})
      {
        now = names_[role].with(now, names_[role].of(old));
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
  regs_.poke(csr_.offset, names_[rcc_role::remove_flags].with(now, 0));
}

template <typename Traits>
void rcc_model<Traits>::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= event_count<Traits>
     or payload != state_.generation[kind]
     or (state_.pending >> kind & 1u) == 0)
  {
    return;
  }
  state_.pending &= ~(std::uint32_t{1} << kind);
  if(kind == divider_kind)
  {
    update(regs_, names_[rcc_role::hsi_divider_flag], 1);
  }
  else if(kind == switch_kind)
  {
    auto const target = read(rcc_role::sw);
    if(auto const input = system_source(target); input and is_ready(*input))
    {
      update(regs_, names_[rcc_role::sws], target & Traits::sw_mask);
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

template <typename Traits> void rcc_model<Traits>::settle(bool notify)
{
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
  if(target == (read(rcc_role::sws) & Traits::sw_mask))
  {
    if((state_.pending >> switch_kind & 1u) != 0)
    {
      cancel(switch_kind);
    }
  }
  else if(
      auto const input = system_source(target);
      input and is_ready(*input) and (state_.pending >> switch_kind & 1u) == 0)
  {
    schedule(switch_kind);
  }
  publish(notify);
}

template <typename Traits>
auto rcc_model<Traits>::rates() const -> std::array<std::uint64_t, signal_count>
{
  std::array<std::uint64_t, signal_count> hz{};
  auto const at    = [&](signal which) -> std::uint64_t& { return hz[std::to_underlying(which)]; };
  auto const ready = [&](source which) { return is_ready(which); };
  rate_inputs const inputs{.regs = regs_, .names = names_, .options = options_};
  for(auto const& [index, names] : std::views::enumerate(Traits::oscillators))
  {
    if(names.rate != nullptr)
    {
      at(names.out) = ready(static_cast<source>(index)) ? names.rate(inputs) : 0;
    }
  }
  for(auto const& node : Traits::divided)
  {
    at(node.out) = at(node.in) / node.divider;
  }
  for(std::size_t pll = 0; pll < pll_.size(); ++pll)
  {
    auto const& fields = pll_[pll];
    auto const& node   = Traits::pll_nodes[pll];
    auto const  which  = static_cast<source>(std::to_underlying(source::pll1) + pll);
    auto const  input  = at(Traits::pll_sources[get(regs_, fields.src)]);
    auto const  value  = [&](std::optional<field_ref> const& f) {
      return f ? get(regs_, *f) : std::uint32_t{};
    };
    auto const frac    = fields.fracen and value(fields.fracen) != 0 ? value(fields.frac) : 0u;
    auto const divisor = fields.m ? node.m_decode(get(regs_, *fields.m)) : 1;
    auto const vco =
        ready(which) and divisor != 0
            ? input * (node.n_decode(get(regs_, fields.n)) * frac_one + frac) / (divisor * frac_one)
            : 0;
    for(std::size_t output = 0; output < node.outputs; ++output)
    {
      auto const& enable = fields.output_enable[output];
      at(node.out[output]) =
          not enable or get(regs_, *enable) != 0
              ? vco / node.output_decode[output](get(regs_, fields.divider[output]))
              : 0;
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
  rate_inputs const inputs{.regs = regs_, .names = names_, .options = options_};
  for(auto const& entry : timers_)
  {
    auto const bus      = hz[std::to_underlying(entry.bus)];
    auto const multiple = Traits::timer_multiplier(inputs, entry.prescaler);
    set(entry.domain, get(regs_, entry.enable) != 0 ? bus * multiple : 0);
  }
  auto const core = hz[std::to_underlying(signal::hclk)];
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

template <typename Traits> void rcc_model<Traits>::reset(register_file& regs, reset_kind kind)
{
  auto const kept_bdcr  = regs.peek(bdcr_.offset);
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
  if(kind != reset_kind::power_on)
  {
    regs.poke(bdcr_.offset, kept_bdcr);
    for(auto const& flag : flags_)
    {
      csr = flag.with(csr, 0);
    }
    csr |= kept_flags;
  }
  for(auto const name : Traits::reset_flags_of(kind))
  {
    if(not name.empty())
    {
      csr = field(Traits::reset_flags_register, name).with(csr, 1);
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
  for(auto const name : Traits::unit_registers)
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
      if(auto const alias = std::ranges::find(Traits::unit_aliases, stem, &unit_alias::first);
         alias != Traits::unit_aliases.end())
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
    secured |= names_[rcc_role::backup_reset].mask();
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
  kernel_enables_.clear();
  for(auto const& entry : kernels_)
  {
    kernel_enables_.push_back(get(regs_, entry.enable) != 0 ? 1 : 0);
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
  if constexpr(not Traits::programming_clock.has_value())
  {
    static_cast<void>(held);
  }
  else
  {
    auto const hsi    = *Traits::programming_clock;
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
}
