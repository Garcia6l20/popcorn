#include "factories.hpp"
#include "model_base.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/bus_reset.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
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
constexpr std::uint32_t pe_key1       = 0x89ab'cdef;
constexpr std::uint32_t pe_key2       = 0x0203'0405;
constexpr std::uint32_t program_key1  = 0x8c9d'aebf;
constexpr std::uint32_t program_key2  = 0x1314'1516;
constexpr std::uint32_t option_key1   = 0xfbea'd9c8;
constexpr std::uint32_t option_key2   = 0x2425'2627;
constexpr std::uint32_t power_key1    = 0x0415'2637;
constexpr std::uint32_t power_key2    = 0xfafb'fcfd;
constexpr std::uint32_t operation_end = 0;
constexpr std::uint8_t  power_armed   = 2;
constexpr std::uint32_t lowest_range  = 2;
constexpr std::uint64_t hz_per_khz    = 1000;
constexpr auto          tprog         = std::chrono::nanoseconds{3'280'000};

struct flash_store
{
  std::uint32_t addr{};
  std::uint8_t  size{};
  std::uint32_t value{};

  friend auto operator==(flash_store const&, flash_store const&) -> bool = default;
};

struct flash_l1_state
{
  std::uint8_t  pe_keys{};
  std::uint8_t  program_keys{};
  std::uint8_t  option_keys{};
  std::uint8_t  power_keys{};
  bool          pe_locked_up{};
  bool          program_locked_up{};
  bool          option_locked_up{};
  std::uint32_t generation{};
  std::uint32_t queued{};

  friend auto operator==(flash_l1_state const&, flash_l1_state const&) -> bool = default;
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
 *        Program memory word write, RM0038 3.4.3: a CPU or DMA word store to a Flash region the
 *        description gives FLASH as controller writes the word as given and holds BSY, with
 *        ENDHV clear, for one tprog of 3.28 ms. At its end BSY clears and ENDHV and EOP set. A
 *        store while PELOCK or PRGLOCK is set sets WRPERR, a byte or halfword store SIZERR, a
 *        store at an address not word aligned PGAERR, and none of them programs. A store with
 *        ERASE or FPRG set logs a model_warning once, raises model_error under
 *        strict_peripherals, and programs nothing. A store in voltage range 3 programs and logs
 *        a model_warning once, model_error under strict_peripherals, RM0038 3.6. A store while
 *        BSY is set is decided and written at once and adds one tprog to BSY, EOP setting at the
 *        end of each. The core runs on during BSY. A store to a data EEPROM or option byte region
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
  state_                = {};
  state_.generation     = generation;
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
  if(control_.pe_lock.of(control) != 0 or control_.program_lock.of(control) != 0)
  {
    fail(status_.protection_error);
    return;
  }
  if(control_.erase.of(control) != 0 or control_.half_page.of(control) != 0)
  {
    note_once(
        mode_logged_, "PECR",
        std::format(
            "a store to 0x{:08x} with ERASE or FPRG set, page erase and half-page programming: "
            "nothing is programmed",
            word.addr));
    return;
  }
  if(word.size != 4)
  {
    fail(status_.size_error);
    return;
  }
  if(word.addr % 4 != 0)
  {
    fail(status_.alignment_error);
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
  auto const bytes = std::bit_cast<std::array<std::byte, 4>>(word.value);
  static_cast<void>(ctx_.services.write_block(word.addr, bytes, initiator::debugger));
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
