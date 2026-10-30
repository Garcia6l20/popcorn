#include "factories.hpp"
#include "model_base.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/bus_reset.hpp>
#include <popcorn/periph/firewall.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t pe_key1         = 0x89ab'cdef;
constexpr std::uint32_t pe_key2         = 0x0203'0405;
constexpr std::uint32_t program_key1    = 0x8c9d'aebf;
constexpr std::uint32_t program_key2    = 0x1314'1516;
constexpr std::uint32_t option_key1     = 0xfbea'd9c8;
constexpr std::uint32_t option_key2     = 0x2425'2627;
constexpr std::uint32_t power_key1      = 0x0415'2637;
constexpr std::uint32_t power_key2      = 0xfafb'fcfd;
constexpr std::uint32_t operation_end   = 0;
constexpr std::uint8_t  power_armed     = 2;
constexpr std::uint32_t lowest_range    = 2;
constexpr std::uint64_t hz_per_khz      = 1000;
constexpr std::uint32_t page_bytes      = 256;
constexpr std::uint32_t half_page_words = 32;
constexpr auto          tprog           = std::chrono::nanoseconds{3'280'000};

struct flash_store
{
  std::uint32_t addr{};
  std::uint8_t  size{};
  std::uint32_t value{};

  friend auto operator==(flash_store const&, flash_store const&) -> bool = default;
};

struct flash_l1_state
{
  std::uint8_t                 pe_keys{};
  std::uint8_t                 program_keys{};
  std::uint8_t                 option_keys{};
  std::uint8_t                 power_keys{};
  bool                         pe_locked_up{};
  bool                         program_locked_up{};
  bool                         option_locked_up{};
  std::uint32_t                generation{};
  std::uint32_t                queued{};
  std::vector<std::uint32_t>   loaded{};
  std::uint32_t                first_half{};
  std::uint32_t                second_half{};
  std::optional<std::uint32_t> erase_first{};
  std::uint8_t                 locked{};
  std::uint32_t                lock_ends{};

  friend auto operator==(flash_l1_state const&, flash_l1_state const&) -> bool = default;
};

/**
 * @brief The memory filter of the STM32L1 FLASH interface, installed by its model: while a bank
 *        is locked by a half page write or a parallel operation, a CPU or DMA load of it is a
 *        bus error and a fetch from it a BusFault, RM0038 3.4.3 note 2 and Table 15 note 3.
 *        Stores reach the FLASH model, debugger accesses and every other address pass.
 */
class bank_filter final : public bus_firewall
{
public:
  bank_filter(
      std::span<region_desc const* const> regions,
      std::optional<std::uint32_t>        second_bank,
      std::uint8_t const&                 locked)
      : second_bank_{second_bank}, locked_{locked}
  {
    for(auto const* const region : regions)
    {
      windows_.emplace_back(region->start, region->size);
    }
  }

  [[nodiscard]] auto check_peripheral(std::size_t, bus_attrs, arch::memory_op)
      -> bus_verdict override
  {
    return bus_verdict::allow;
  }

  [[nodiscard]] auto filters_memory() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto memory_page(address, std::uint32_t, address at, bus_attrs, arch::memory_op)
      -> page_verdict override
  {
    if(not locked(at))
    {
      return {};
    }
    return {.filter_data = true, .deny_fetch = true};
  }

  [[nodiscard]] auto check_memory(address at, std::uint8_t, bus_attrs attrs, arch::memory_op op)
      -> bus_verdict override
  {
    if(op == arch::memory_op::write or attrs.by == initiator::debugger or not locked(at))
    {
      return bus_verdict::allow;
    }
    return bus_verdict::bus_error;
  }

  [[nodiscard]] auto allowed_extent(address at, std::uint8_t, bus_attrs, arch::memory_op)
      -> memory_extent override
  {
    if(locked_ == 0)
    {
      return {.first = 0, .last = ~address{}};
    }
    std::uint64_t first = 0;
    std::uint64_t last  = ~address{};
    for(auto const& [start, size] : windows_)
    {
      auto const end = std::uint64_t{start} + size;
      if(at >= start and at < end)
      {
        if(second_bank_ and at >= *second_bank_)
        {
          return {.first = *second_bank_, .last = static_cast<address>(end - 1)};
        }
        auto const top = second_bank_ and *second_bank_ < end ? std::uint64_t{*second_bank_} : end;
        return {.first = start, .last = static_cast<address>(top - 1)};
      }
      if(end <= at)
      {
        first = std::max(first, end);
      }
      else
      {
        last = std::min<std::uint64_t>(last, start - 1u);
      }
    }
    return {.first = static_cast<address>(first), .last = static_cast<address>(last)};
  }

private:
  [[nodiscard]] auto locked(address at) const -> bool
  {
    if(locked_ == 0)
    {
      return false;
    }
    auto const inside = std::ranges::any_of(
        windows_, [at](auto const& w) { return at >= w.first and at - w.first < w.second; });
    auto const bank = second_bank_ and at >= *second_bank_ ? 2u : 1u;
    return inside and (locked_ & bank) != 0;
  }

  std::optional<std::uint32_t>                   second_bank_;
  std::uint8_t const&                            locked_;
  std::vector<std::pair<address, std::uint32_t>> windows_;
};

/** @brief FLASH_PECR fields, RM0038 3.9.2. */
struct control_fields
{
  field_ref pe_lock;
  field_ref program_lock;
  field_ref option_lock;
  field_ref program;
  field_ref data;
  field_ref fixed_time;
  field_ref erase;
  field_ref half_page;
  field_ref parallel_bank;
  field_ref end_irq;
  field_ref error_irq;
  field_ref option_launch;
};

/** @brief FLASH_SR fields, RM0038 3.9.7. */
struct status_fields
{
  field_ref busy;
  field_ref end;
  field_ref high_voltage_end;
  field_ref ready;
  field_ref protection_error;
  field_ref alignment_error;
  field_ref size_error;
  field_ref option_error;
  field_ref user_option_error;

  [[nodiscard]] auto errors() const -> std::uint32_t
  {
    return protection_error.mask()
         | alignment_error.mask()
         | size_error.mask()
         | option_error.mask()
         | user_option_error.mask();
  }
};

/**
 * @brief FLASH and data EEPROM interface of the STM32L1, RM0038 section 3, the category
 *        differences from the traits.
 *
 *        Locks, RM0038 3.4.1: FLASH_PECR.PELOCK, PRGLOCK and OPTLOCK are set after every reset.
 *        PEKEY1 then PEKEY2 in FLASH_PEKEYR clears PELOCK, PRGKEY1 then PRGKEY2 in FLASH_PRGKEYR
 *        clears PRGLOCK, OPTKEY1 then OPTKEY2 in FLASH_OPTKEYR clears OPTLOCK. A wrong key, a third
 *        write, or a write to FLASH_PRGKEYR or FLASH_OPTKEYR while PELOCK is set is a bus error
 *        that sets the lock it targets, which then stays set until the next reset. The key
 *        registers read 0. While PELOCK is set a FLASH_PECR write changes nothing. The lock bits
 *        are set by a write of 1 only, and setting PELOCK sets PRGLOCK and OPTLOCK and clears PROG,
 *        DATA, FTDW, ERASE and FPRG. PROG, DATA, FTDW, ERASE, FPRG and PARALLELBANK keep their
 *        value while BSY is set, PARALLELBANK reads 0 on a part with one bank. OBL_LAUNCH is stored
 *        while OPTLOCK is clear and loads no option byte.
 *
 *        FLASH_ACR, RM0038 3.9.1: LATENCY and PRFTEN take a write only while ACC64 is set and the
 *        write leaves ACC64 unchanged. RUN_PD takes the first FLASH_ACR write after PDKEY1 then
 *        PDKEY2 in FLASH_PDKEYR, and FLASH_SR.READY reads the inverse of RUN_PD. A LATENCY below
 *        Table 13 for HCLK and the voltage range of the PWR peer, or an HCLK above the range,
 *        logs a model_warning once per HCLK, range and LATENCY at a FLASH_ACR write, a clock
 *        change or a range change, and raises model_error under strict_peripherals.
 *
 *        Program memory, RM0038 3.4.2, 3.4.3 and Tables 20 and 21: a CPU or DMA word store to a
 *        Flash region the description gives FLASH as controller, with ERASE and FPRG clear,
 *        writes the word as given. With ERASE and PROG set, a store to the first word of a
 *        256-byte page erases the page to 0. With FPRG and PROG set, 32 word stores from word 0
 *        to word 31 of a 128-byte half page program it after the last one. With PARALLELBANK
 *        set, on a part with two banks, a page of bank 1 then a page of bank 2 erase together,
 *        and a half page of bank 1 then one of bank 2, 64 stores, program together. Each
 *        operation holds BSY, with ENDHV clear, for one tprog of 3.28 ms. At its end BSY clears
 *        and ENDHV and EOP set. A store while PELOCK or PRGLOCK is set sets WRPERR, a byte or
 *        halfword store SIZERR, and none of them programs. A store at an address not word
 *        aligned, an erase not at a page start, a half page store out of sequence, or a page or
 *        half page of the wrong bank sets PGAERR and drops the loaded sequence. A FLASH_PECR
 *        write that changes PROG, ERASE, FPRG or PARALLELBANK, or sets PELOCK, drops a sequence
 *        in progress, with PGAERR when half page words were loaded. A store with ERASE or FPRG
 *        set and PROG clear, or with both set, logs a model_warning once, raises model_error
 *        under strict_peripherals, and programs nothing. An operation in voltage range 3
 *        completes and logs a model_warning once, model_error under strict_peripherals, RM0038
 *        3.6. A store while BSY is set is decided against FLASH_PECR at once, writes at once, and
 *        an operation it completes adds one tprog to BSY, EOP setting at the end of each. The
 *        core runs on during BSY, except that from the first store of a half page sequence to
 *        the end of its tprog a CPU or DMA load of the bank written is a bus error and a fetch
 *        from it a BusFault, both banks for a parallel operation, RM0038 3.4.3 note 2, through the
 *        model's bus firewall. While such an operation is BSY a store to the other bank sets
 *        WRPERR and writes nothing, RM0038 3.4.4. A store to a data EEPROM or option byte region
 *        is a bus error.
 *
 *        FLASH_SR: EOP, WRPERR, PGAERR, SIZERR, OPTVERR and OPTVERRUSR clear on a write of 1.
 *        FLASH_IRQn is raised while EOP and EOPIE, or an error flag and ERRIE, are set, RM0038
 *        3.8. Every reset, and an RCC FLITFRST pulse while FLASH_ACR.RUN_PD is set, RM0038
 *        6.3.5, bring the registers to their description reset with READY set, set every lock
 *        and end the operation in flight and the queued ones. A pulse with RUN_PD clear changes
 *        nothing. FLASH_OBR and FLASH_WRPRx are left to the register file.
 */
class flash_l1 final : public dispatch_model<flash_l1, flash_l1_state>,
                       public clock_listener,
                       public voltage_range_listener,
                       public bus_reset_listener
{
public:
  flash_l1(model_context& ctx, peripheral_desc const& desc, flash_l1_traits const& traits);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void load(std::span<std::byte const> in) override;
  void on_clock_change() override;
  void on_voltage_range_change() override;

  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  auto on_memory_write(
      region_desc const& region,
      address            addr,
      std::uint8_t       size,
      std::uint32_t      value,
      access_ctx         ctx) -> std::expected<void, bus_error> override;

  void access_written(std::uint32_t old, std::uint32_t now);
  void control_written(std::uint32_t old, std::uint32_t now);
  void power_key_written(std::uint32_t old, std::uint32_t now);
  void pe_key_written(std::uint32_t old, std::uint32_t now);
  void program_key_written(std::uint32_t old, std::uint32_t now);
  void option_key_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto range() const -> std::uint32_t;
  void               restore(reset_kind kind);
  void               lock(std::uint32_t& control);
  void               program(flash_store const& word);
  void               erase_page(std::uint32_t addr);
  void               load_half_page(std::uint32_t addr, std::uint32_t value);
  void               start(std::span<std::byte const> bytes, std::uint32_t at);
  void               abort_sequence();
  void               lock_banks(std::uint8_t banks);
  void               release_banks();
  [[nodiscard]] auto bank_of(std::uint32_t addr) const -> std::uint8_t;
  void               begin_locked();
  [[nodiscard]] auto in_second_bank(std::uint32_t addr) const -> bool;
  [[nodiscard]] auto parallel() const -> bool;
  void               begin();
  void               fail(field_ref flag);
  void               finish();
  void               interrupts();
  void               check_latency(std::string_view trigger);
  void               note_once(bool& logged, std::string_view reg, std::string message);
  auto               key_step(
      std::uint8_t& keys,
      bool&         locked_up,
      bool          armed,
      field_ref     lock,
      std::uint32_t key1,
      std::uint32_t key2,
      std::uint32_t now) -> bool;

  register_file&                                                    regs_;
  flash_l1_traits                                                   traits_;
  field_ref                                                         latency_;
  field_ref                                                         prefetch_;
  field_ref                                                         wide_;
  field_ref                                                         sleep_down_;
  field_ref                                                         run_down_;
  control_fields                                                    control_;
  status_fields                                                     status_;
  std::array<reg_ref, 4>                                            keys_{};
  std::optional<std::uint32_t>                                      second_bank_;
  std::unique_ptr<bank_filter>                                      filter_;
  irq_line&                                                         line_;
  core::clock_domain                                                hclk_;
  std::uint32_t                                                     written_{};
  bool                                                              rejected_{};
  bool                                                              mode_logged_{};
  bool                                                              range_logged_{};
  std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> latency_logged_;
};

constexpr std::array<write_entry<flash_l1>, 7> flash_l1_writes{
    {
     {"ACR", &flash_l1::access_written},
     {"PECR", &flash_l1::control_written},
     {"PDKEYR", &flash_l1::power_key_written},
     {"PEKEYR", &flash_l1::pe_key_written},
     {"PRGKEYR", &flash_l1::program_key_written},
     {"OPTKEYR", &flash_l1::option_key_written},
     {"SR", &flash_l1::status_written},
     }
};

flash_l1::flash_l1(model_context& ctx, peripheral_desc const& desc, flash_l1_traits const& traits)
    : dispatch_model{ctx, desc, "FLASH", flash_l1_writes}, regs_{ctx.regs}, traits_{traits},
      latency_{field("ACR", "LATENCY")}, prefetch_{field("ACR", "PRFTEN")},
      wide_{field("ACR", "ACC64")}, sleep_down_{field("ACR", "SLEEP_PD")},
      run_down_{field("ACR", "RUN_PD")},
      control_{
          .pe_lock       = field("PECR", "PELOCK"),
          .program_lock  = field("PECR", "PRGLOCK"),
          .option_lock   = field("PECR", "OPTLOCK"),
          .program       = field("PECR", "PROG"),
          .data          = field("PECR", "DATA"),
          .fixed_time    = field("PECR", "FTDW"),
          .erase         = field("PECR", "ERASE"),
          .half_page     = field("PECR", "FPRG"),
          .parallel_bank = field("PECR", "PARALLELBANK"),
          .end_irq       = field("PECR", "EOPIE"),
          .error_irq     = field("PECR", "ERRIE"),
          .option_launch = field("PECR", "OBL_LAUNCH")},
      status_{
          .busy              = field("SR", "BSY"),
          .end               = field("SR", "EOP"),
          .high_voltage_end  = field("SR", "ENDHV"),
          .ready             = field("SR", "READY"),
          .protection_error  = field("SR", "WRPERR"),
          .alignment_error   = field("SR", "PGAERR"),
          .size_error        = field("SR", "SIZERR"),
          .option_error      = field("SR", "OPTVERR"),
          .user_option_error = field("SR", "OPTVERRUSR")},
      keys_{reg("PDKEYR"), reg("PEKEYR"), reg("PRGKEYR"), reg("OPTKEYR")}, line_{irq("FLASH_IRQn")},
      hclk_{ctx.clock.domain("hclk")}
{
  for(std::uint8_t index = 1; index <= 4; ++index)
  {
    expect_register(std::format("WRPR{}", index), index <= traits.wrp_registers);
  }
  std::vector<region_desc const*> program_regions;
  for(auto const& region : ctx.part.regions)
  {
    if(region.controller == desc.name and region.acc.execute and not region.physical)
    {
      program_regions.push_back(&region);
    }
  }
  std::ranges::sort(program_regions, {}, &region_desc::start);
  if(traits.dual_bank and program_regions.size() == 1)
  {
    second_bank_ =
        static_cast<std::uint32_t>(program_regions[0]->start + program_regions[0]->size / 2);
  }
  else if(traits.dual_bank and program_regions.size() == 2)
  {
    second_bank_ = static_cast<std::uint32_t>(program_regions[1]->start);
  }
  else if(traits.dual_bank)
  {
    throw config_error(
        ctx.part.part, 0, "regions",
        "one or two Flash regions that name FLASH as their controller, the two banks");
  }
  filter_ = std::make_unique<bank_filter>(program_regions, second_bank_, state_.locked);
  ctx.install_firewall(*filter_);
}

auto flash_l1::bank_of(std::uint32_t addr) const -> std::uint8_t
{
  return in_second_bank(addr) ? 2 : 1;
}

void flash_l1::lock_banks(std::uint8_t banks)
{
  if((state_.locked | banks) == state_.locked)
  {
    return;
  }
  state_.locked |= banks;
  ctx_.services.memory_policy_changed();
  ctx_.services.end_after_instruction();
}

void flash_l1::release_banks()
{
  if(state_.locked == 0 or state_.lock_ends != 0 or not state_.loaded.empty() or state_.erase_first)
  {
    return;
  }
  state_.locked = 0;
  ctx_.services.memory_policy_changed();
  ctx_.services.end_after_instruction();
}

void flash_l1::begin_locked()
{
  auto const ends  = busy() ? state_.queued + 2 : 1;
  state_.lock_ends = std::max(state_.lock_ends, ends);
  begin();
}

auto flash_l1::in_second_bank(std::uint32_t addr) const -> bool
{
  return second_bank_ and addr >= *second_bank_;
}

auto flash_l1::parallel() const -> bool
{
  return get(regs_, control_.parallel_bank) != 0;
}

void flash_l1::abort_sequence()
{
  state_.loaded.clear();
  state_.erase_first.reset();
  release_banks();
}

void flash_l1::start(std::span<std::byte const> bytes, std::uint32_t at)
{
  static_cast<void>(ctx_.services.write_block(at, bytes, initiator::debugger));
}

void flash_l1::erase_page(std::uint32_t addr)
{
  if(addr % page_bytes != 0)
  {
    abort_sequence();
    fail(status_.alignment_error);
    return;
  }
  std::array<std::byte, page_bytes> const erased{};
  if(not parallel())
  {
    start(erased, addr);
  }
  else if(not state_.erase_first)
  {
    if(in_second_bank(addr))
    {
      fail(status_.alignment_error);
      return;
    }
    state_.erase_first = addr;
    lock_banks(3);
    return;
  }
  else
  {
    if(not in_second_bank(addr))
    {
      abort_sequence();
      fail(status_.alignment_error);
      return;
    }
    start(erased, *std::exchange(state_.erase_first, std::nullopt));
    start(erased, addr);
    begin_locked();
    return;
  }
  begin();
}

void flash_l1::load_half_page(std::uint32_t addr, std::uint32_t value)
{
  auto const count = static_cast<std::uint32_t>(state_.loaded.size());
  auto const half  = half_page_words * 4;
  auto       ok    = true;
  if(count == 0)
  {
    ok                = addr % half == 0 and not(parallel() and in_second_bank(addr));
    state_.first_half = addr;
  }
  else if(count == half_page_words)
  {
    ok                 = addr % half == 0 and in_second_bank(addr);
    state_.second_half = addr;
  }
  else
  {
    auto const base = count < half_page_words ? state_.first_half : state_.second_half;
    ok              = addr == base + 4 * (count % half_page_words);
  }
  if(not ok)
  {
    abort_sequence();
    fail(status_.alignment_error);
    return;
  }
  state_.loaded.push_back(value);
  lock_banks(parallel() ? 3 : bank_of(state_.first_half));
  auto const total = parallel() ? 2 * half_page_words : half_page_words;
  if(state_.loaded.size() < total)
  {
    return;
  }
  auto const words = std::exchange(state_.loaded, {});
  auto const bytes = std::as_bytes(std::span{words});
  start(bytes.first(half), state_.first_half);
  if(words.size() > half_page_words)
  {
    start(bytes.subspan(half), state_.second_half);
  }
  begin_locked();
}

auto flash_l1::busy() const -> bool
{
  return get(regs_, status_.busy) != 0;
}

auto flash_l1::range() const -> std::uint32_t
{
  std::uint32_t level = 0;
  for(auto* const peer : ctx_.peers.all())
  {
    if(auto const* const source = dynamic_cast<voltage_range_source const*>(peer))
    {
      level = source->active_level();
    }
  }
  return std::min(level, lowest_range);
}

void flash_l1::restore(reset_kind)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  update(regs_, status_.ready, 1);
  auto const generation = state_.generation;
  auto const locked     = state_.locked;
  state_                = {};
  state_.generation     = generation;
  if(locked != 0)
  {
    ctx_.services.memory_policy_changed();
  }
  interrupts();
}

void flash_l1::reset(register_file&, reset_kind kind)
{
  restore(kind);
}

void flash_l1::on_bus_reset()
{
  if(get(regs_, run_down_) != 0)
  {
    restore(reset_kind::peripheral);
  }
}

void flash_l1::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  ctx_.services.memory_policy_changed();
  latency_logged_.clear();
  mode_logged_  = false;
  range_logged_ = false;
}

void flash_l1::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == operation_end and payload == state_.generation and busy())
  {
    finish();
  }
}

void flash_l1::finish()
{
  update(regs_, status_.end, 1);
  if(state_.lock_ends != 0 and --state_.lock_ends == 0)
  {
    release_banks();
  }
  if(state_.queued != 0)
  {
    --state_.queued;
    schedule_in(tprog, operation_end, ++state_.generation);
  }
  else
  {
    update(regs_, status_.busy, 0);
    update(regs_, status_.high_voltage_end, 1);
  }
  interrupts();
}

void flash_l1::interrupts()
{
  auto const status  = regs_.peek(status_.busy.offset);
  auto const control = regs_.peek(control_.end_irq.offset);
  auto const raised  = (status_.end.of(status) != 0 and control_.end_irq.of(control) != 0)
                    or ((status & status_.errors()) != 0 and control_.error_irq.of(control) != 0);
  if(raised)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void flash_l1::fail(field_ref flag)
{
  update(regs_, flag, 1);
  interrupts();
}

void flash_l1::note_once(bool& logged, std::string_view reg, std::string message)
{
  if(not std::exchange(logged, true) or ctx_.strict_peripherals)
  {
    warn(reg, std::move(message));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{reg}, this->reg(reg).offset);
  }
}

void flash_l1::program(flash_store const& word)
{
  auto const control = regs_.peek(control_.pe_lock.offset);
  if(control_.pe_lock.of(control) != 0
     or control_.program_lock.of(control) != 0
     or (busy() and state_.lock_ends != 0 and (state_.locked & bank_of(word.addr)) == 0))
  {
    fail(status_.protection_error);
    return;
  }
  if(word.size != 4)
  {
    abort_sequence();
    fail(status_.size_error);
    return;
  }
  if(word.addr % 4 != 0)
  {
    abort_sequence();
    fail(status_.alignment_error);
    return;
  }
  auto const erase     = control_.erase.of(control) != 0;
  auto const half_page = control_.half_page.of(control) != 0;
  if((erase or half_page) and (erase == half_page or control_.program.of(control) == 0))
  {
    note_once(
        mode_logged_, "PECR",
        std::format(
            "a store to 0x{:08x} with ERASE or FPRG set and PROG clear, or with both set: "
            "nothing is programmed",
            word.addr));
    return;
  }
  if(range() == lowest_range)
  {
    note_once(
        range_logged_, "PECR",
        std::format(
            "a program of 0x{:08x} in voltage range 3, RM0038 3.6 allows programming in ranges 1 "
            "and 2 only",
            word.addr));
  }
  if(erase)
  {
    erase_page(word.addr);
    return;
  }
  if(half_page)
  {
    load_half_page(word.addr, word.value);
    return;
  }
  start(std::bit_cast<std::array<std::byte, 4>>(word.value), word.addr);
  begin();
}

void flash_l1::begin()
{
  if(busy())
  {
    ++state_.queued;
    return;
  }
  update(regs_, status_.busy, 1);
  update(regs_, status_.high_voltage_end, 0);
  schedule_in(tprog, operation_end, ++state_.generation);
}

auto flash_l1::on_memory_write(
    region_desc const& region,
    address            addr,
    std::uint8_t       size,
    std::uint32_t      value,
    access_ctx) -> std::expected<void, bus_error>
{
  if(not region.acc.execute)
  {
    return std::unexpected{bus_error::denied};
  }
  program({.addr = static_cast<std::uint32_t>(addr), .size = size, .value = value});
  return {};
}

auto flash_l1::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  written_        = merge_write(0, offset, size, value);
  auto const done = dispatch_model::on_write(regs, offset, size, old, value, ctx);
  if(std::exchange(rejected_, false))
  {
    return std::unexpected{bus_error::denied};
  }
  return done;
}

void flash_l1::access_written(std::uint32_t old, std::uint32_t now)
{
  auto word = wide_.with(old, wide_.of(now));
  word      = sleep_down_.with(word, sleep_down_.of(now));
  if(wide_.of(old) != 0 and wide_.of(now) != 0)
  {
    word = latency_.with(word, latency_.of(now));
    word = prefetch_.with(word, prefetch_.of(now));
  }
  if(std::exchange(state_.power_keys, 0) == power_armed)
  {
    word = run_down_.with(word, run_down_.of(now));
  }
  regs_.poke(latency_.offset, word);
  update(regs_, status_.ready, run_down_.of(word) == 0 ? 1 : 0);
  check_latency("on a FLASH_ACR write");
}

void flash_l1::lock(std::uint32_t& control)
{
  control = control_.pe_lock.with(control, 1);
  control = control_.program_lock.with(control, 1);
  control = control_.option_lock.with(control, 1);
  for(auto const bits :
      {control_.program, control_.data, control_.fixed_time, control_.erase, control_.half_page})
  {
    control = bits.with(control, 0);
  }
  state_.pe_keys      = 0;
  state_.program_keys = 0;
  state_.option_keys  = 0;
  if(not state_.loaded.empty())
  {
    update(regs_, status_.alignment_error, 1);
  }
  abort_sequence();
}

void flash_l1::control_written(std::uint32_t old, std::uint32_t now)
{
  if(control_.pe_lock.of(old) != 0)
  {
    regs_.poke(control_.pe_lock.offset, old);
    return;
  }
  auto word = old;
  for(auto const bits : {control_.end_irq, control_.error_irq})
  {
    word = bits.with(word, bits.of(now));
  }
  if(not busy())
  {
    for(auto const bits :
        {control_.program, control_.data, control_.fixed_time, control_.erase, control_.half_page})
    {
      word = bits.with(word, bits.of(now));
    }
    word =
        control_.parallel_bank.with(word, traits_.dual_bank ? control_.parallel_bank.of(now) : 0u);
    auto const modes = control_.program.mask()
                     | control_.erase.mask()
                     | control_.half_page.mask()
                     | control_.parallel_bank.mask();
    if(((word ^ old) & modes) != 0 and (not state_.loaded.empty() or state_.erase_first))
    {
      if(not state_.loaded.empty())
      {
        update(regs_, status_.alignment_error, 1);
      }
      abort_sequence();
    }
  }
  if(control_.option_lock.of(old) == 0)
  {
    word = control_.option_launch.with(word, control_.option_launch.of(now));
  }
  if(control_.program_lock.of(now) != 0 and control_.program_lock.of(old) == 0)
  {
    word                = control_.program_lock.with(word, 1);
    state_.program_keys = 0;
  }
  if(control_.option_lock.of(now) != 0 and control_.option_lock.of(old) == 0)
  {
    word               = control_.option_lock.with(word, 1);
    state_.option_keys = 0;
  }
  if(control_.pe_lock.of(now) != 0)
  {
    lock(word);
  }
  regs_.poke(control_.pe_lock.offset, word);
  interrupts();
}

void flash_l1::power_key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(keys_[0].offset, 0);
  if(state_.power_keys == 0 and now == power_key1)
  {
    state_.power_keys = 1;
    return;
  }
  state_.power_keys = state_.power_keys == 1 and now == power_key2 ? power_armed : 0;
}

auto flash_l1::key_step(
    std::uint8_t& keys,
    bool&         locked_up,
    bool          armed,
    field_ref     lock,
    std::uint32_t key1,
    std::uint32_t key2,
    std::uint32_t now) -> bool
{
  if(armed and not locked_up and get(regs_, lock) != 0)
  {
    if(keys == 0 and now == key1)
    {
      keys = 1;
      return true;
    }
    if(keys == 1 and now == key2)
    {
      keys = 0;
      update(regs_, lock, 0);
      return true;
    }
  }
  keys      = 0;
  locked_up = true;
  rejected_ = true;
  update(regs_, lock, 1);
  return false;
}

void flash_l1::pe_key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(keys_[1].offset, 0);
  if(not key_step(
         state_.pe_keys, state_.pe_locked_up, true, control_.pe_lock, pe_key1, pe_key2, now))
  {
    auto word = regs_.peek(control_.pe_lock.offset);
    lock(word);
    regs_.poke(control_.pe_lock.offset, word);
    interrupts();
  }
}

void flash_l1::program_key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(keys_[2].offset, 0);
  static_cast<void>(key_step(
      state_.program_keys, state_.program_locked_up, get(regs_, control_.pe_lock) == 0,
      control_.program_lock, program_key1, program_key2, now));
}

void flash_l1::option_key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(keys_[3].offset, 0);
  static_cast<void>(key_step(
      state_.option_keys, state_.option_locked_up, get(regs_, control_.pe_lock) == 0,
      control_.option_lock, option_key1, option_key2, now));
}

void flash_l1::status_written(std::uint32_t old, std::uint32_t)
{
  auto const cleared = (status_.end.mask() | status_.errors()) & written_;
  regs_.poke(status_.busy.offset, old & ~cleared);
  interrupts();
}

void flash_l1::check_latency(std::string_view trigger)
{
  auto const hclk = ctx_.clock.hz(hclk_);
  if(hclk == 0)
  {
    return;
  }
  auto const level   = range();
  auto const latency = get(regs_, latency_);
  auto const limits  = traits_.max_khz[level];
  if(hclk <= limits[latency] * hz_per_khz)
  {
    return;
  }
  if(latency_logged_.emplace(hclk, level, latency).second or ctx_.strict_peripherals)
  {
    warn(
        "ACR", hclk <= limits[1] * hz_per_khz
                   ? std::format(
                         "{}: LATENCY {} is below the 1 wait state RM0038 Table 13 asks for HCLK "
                         "{} Hz in voltage range {}, silicon would read wrong data",
                         trigger, latency, hclk, level + 1)
                   : std::format(
                         "{}: HCLK {} Hz is above the maximum of voltage range {}, RM0038 Table 13",
                         trigger, hclk, level + 1));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, "ACR", latency_.offset);
  }
}

void flash_l1::on_clock_change()
{
  check_latency("after an HCLK change");
}

void flash_l1::on_voltage_range_change()
{
  check_latency("after a voltage range change");
}
}

auto flash_l1_factory(flash_l1_traits traits) -> model_factory
{
  return [traits](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<flash_l1>(ctx, desc, traits);
  };
}
}
