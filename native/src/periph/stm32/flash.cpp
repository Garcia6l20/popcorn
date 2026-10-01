#include "factories.hpp"
#include "hsi16_source.hpp"
#include "model_base.hpp"

#include <popcorn/core/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t key1          = 0x4567'0123;
constexpr std::uint32_t key2          = 0xcdef'89ab;
constexpr std::uint32_t quad_word     = 16;
constexpr std::uint32_t page_size     = 8 * 1024;
constexpr std::uint8_t  all_words     = 0xf;
constexpr std::uint32_t operation_end = 0;

enum class operation : std::uint8_t
{
  none,
  program,
  page_erase,
  bank_erase,
  mass_erase,
};

struct held_store
{
  std::uint32_t addr{};
  std::uint8_t  size{};
  std::uint32_t value{};

  friend auto operator==(held_store const&, held_store const&) -> bool = default;
};

struct flash_state
{
  bool                         first_key{};
  bool                         locked_up{};
  std::array<std::uint32_t, 4> buffer{};
  std::uint8_t                 filled{};
  std::uint32_t                buffer_addr{};
  std::uint8_t                 running{};
  std::uint32_t                target{};
  std::uint32_t                length{};
  std::uint32_t                generation{};
  std::vector<held_store>      held{};

  friend auto operator==(flash_state const&, flash_state const&) -> bool = default;
};

void drive(irq_line& line, bool level)
{
  if(level)
  {
    line.raise();
  }
  else
  {
    line.lower();
  }
}

auto all_of_byte(std::span<std::byte const> bytes, std::byte value) -> bool
{
  return std::ranges::all_of(bytes, [value](std::byte b) { return b == value; });
}

/**
 * @brief FLASH interface of the STM32U5, non-secure registers, RM0456 section 7.
 *
 * Unlock, RM0456 7.3.5: KEY1 then KEY2 in FLASH_NSKEYR clears FLASH_NSCR.LOCK, any other write
 * is a bus error and keeps FLASH_NSCR locked until the next reset. A key write while unlocked
 * counts as a wrong sequence. FLASH_NSCR ignores writes while locked. Option-byte unlock is not
 * modelled: OPTLOCK stays 1 and OPTSTRT is cleared.
 *
 * Program, RM0456 7.3.7: with PG set, a 32-bit store at a quad-word address opens the write
 * buffer and sets WDW, the fourth word of that quad-word launches the program, BSY is set for
 * flash_options::quad_word_program, then the 16 bytes are written. Erase, RM0456 7.3.6: STRT
 * with PER erases the page PNB of the bank BKER, with MER1 and MER2 one or both banks, BSY set
 * for the flash_options time, then the range reads 0xFF and STRT clears. EOP is set at the end
 * only with EOPIE. Geometry: option bytes are not modelled, so the model uses the production
 * FLASH_OPTR value 0x1FEFF8AA, RM0456 7.9.13: DUALBANK = 1, SWAP_BANK = 0, bank 2 at the flash
 * base plus half its size, 8 Kbyte pages. A PNB at or above the pages of a bank, which RM0456
 * leaves undefined, erases nothing, sets no flag and logs a model_warning once, and raises
 * model_error under strict_peripherals.
 *
 * Errors, RM0456 7.3.9: PGSERR, SIZERR, PGAERR and PROGERR as its list states. For PROGERR the
 * 7.3.9 list wins over the FLASH_NSSR bit text, which accepts all-zero data over an all-zero
 * target: the model refuses it. Every error aborts the operation and flushes the write buffer.
 * With ERRIE an error also sets OPERR. FLASH_IRQn follows EOP with EOPIE and OPERR with ERRIE.
 * WRPERR is not modelled: write protection needs option bytes, logged once. Burst programming,
 * BWR, is logged once and programs quad-word by quad-word. HSI16 is kept on while PG or STRT is
 * set, through the RCC model.
 *
 * Deviations: a store into flash while BSY is set stalls the bus on silicon until BSY clears.
 * The model lets the store complete at once and applies it by the rules above when BSY clears,
 * so the next instructions run earlier than on silicon. The held stores have no bound: a store
 * stream during a 195 ms bank erase keeps every word until the erase ends. Code fetches from a
 * bank being erased or programmed also stall on silicon, RM0456 7.3.10, the model keeps
 * executing without a stall. STRT written while BSY is set, which RM0456 leaves undefined, is
 * ignored: STRT stays set and no flag is raised.
 */
class flash final : public dispatch_model<flash, flash_state>
{
public:
  flash(model_context& ctx, peripheral_desc const& desc, flash_options const& options);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
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

  void key_written(std::uint32_t old, std::uint32_t now);
  void control_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto errors_pending() const -> bool;
  void               fail(field_ref flag);
  void               interrupt();
  void               finish();
  void               store(std::uint32_t addr, std::uint8_t size, std::uint32_t value);
  void               start(operation which, std::uint32_t target, std::uint32_t length);
  void               start_erase(std::uint32_t control);
  void               program();
  void               hold_hsi16(bool held);
  void               note_unmodelled_protection();

  flash_options      options_;
  register_file&     regs_;
  region_desc const* storage_{};
  reg_ref            key_;
  reg_ref            status_;
  field_ref          lock_;
  field_ref          option_lock_;
  field_ref          start_;
  field_ref          option_start_;
  field_ref          program_;
  field_ref          page_erase_;
  field_ref          bank1_erase_;
  field_ref          bank2_erase_;
  field_ref          page_;
  field_ref          bank_;
  field_ref          burst_;
  field_ref          end_irq_;
  field_ref          error_irq_;
  field_ref          end_;
  field_ref          operation_error_;
  field_ref          program_error_;
  field_ref          protection_error_;
  field_ref          alignment_error_;
  field_ref          size_error_;
  field_ref          sequence_error_;
  field_ref          option_error_;
  field_ref          busy_;
  field_ref          waiting_;
  irq_line&          line_;
  std::uint32_t      clearable_{};
  bool               rejected_{};
  bool               page_logged_{};
  bool               burst_logged_{};
  bool               protection_logged_{};
};

constexpr std::array<write_entry<flash>, 3> flash_writes{
    {
     {"FLASH_NSKEYR", &flash::key_written},
     {"FLASH_NSCR", &flash::control_written},
     {"FLASH_NSSR", &flash::status_written},
     }
};

flash::flash(model_context& ctx, peripheral_desc const& desc, flash_options const& options)
    : dispatch_model{ctx, desc, "FLASH", flash_writes}, options_{options}, regs_{ctx.regs},
      key_{reg("FLASH_NSKEYR")}, status_{reg("FLASH_NSSR")}, lock_{field("FLASH_NSCR", "LOCK")},
      option_lock_{field("FLASH_NSCR", "OPTLOCK")}, start_{field("FLASH_NSCR", "STRT")},
      option_start_{field("FLASH_NSCR", "OPTSTRT")}, program_{field("FLASH_NSCR", "PG")},
      page_erase_{field("FLASH_NSCR", "PER")}, bank1_erase_{field("FLASH_NSCR", "MER1")},
      bank2_erase_{field("FLASH_NSCR", "MER2")}, page_{field("FLASH_NSCR", "PNB")},
      bank_{field("FLASH_NSCR", "BKER")}, burst_{field("FLASH_NSCR", "BWR")},
      end_irq_{field("FLASH_NSCR", "EOPIE")}, error_irq_{field("FLASH_NSCR", "ERRIE")},
      end_{field("FLASH_NSSR", "EOP")}, operation_error_{field("FLASH_NSSR", "OPERR")},
      program_error_{field("FLASH_NSSR", "PROGERR")},
      protection_error_{field("FLASH_NSSR", "WRPERR")},
      alignment_error_{field("FLASH_NSSR", "PGAERR")}, size_error_{field("FLASH_NSSR", "SIZERR")},
      sequence_error_{field("FLASH_NSSR", "PGSERR")}, option_error_{field("FLASH_NSSR", "OPTWERR")},
      busy_{field("FLASH_NSSR", "BSY")}, waiting_{field("FLASH_NSSR", "WDW")},
      line_{irq("FLASH_IRQn")}
{
  for(auto const flag :
      {end_, operation_error_, program_error_, protection_error_, alignment_error_, size_error_,
       sequence_error_, option_error_})
  {
    clearable_ |= flag.mask();
  }
  auto const own = std::ranges::find_if(ctx.part.regions, [&desc](region_desc const& region) {
    return region.controller == desc.name and not region.physical;
  });
  if(own != ctx.part.regions.end())
  {
    storage_ = &*own;
  }
}

void flash::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation;
  state_                = {};
  state_.generation     = generation + 1;
  line_.lower();
}

auto flash::busy() const -> bool
{
  return static_cast<operation>(state_.running) != operation::none;
}

auto flash::errors_pending() const -> bool
{
  return std::ranges::any_of(
      std::array{
          program_error_, size_error_, alignment_error_, protection_error_, sequence_error_,
          option_error_},
      [this](field_ref flag) { return get(regs_, flag) != 0; });
}

void flash::interrupt()
{
  drive(
      line_, (get(regs_, end_) != 0 and get(regs_, end_irq_) != 0)
                 or (get(regs_, operation_error_) != 0 and get(regs_, error_irq_) != 0));
}

void flash::fail(field_ref flag)
{
  update(regs_, flag, 1);
  if(get(regs_, error_irq_) != 0)
  {
    update(regs_, operation_error_, 1);
  }
  state_.filled = 0;
  update(regs_, waiting_, 0);
  if(busy())
  {
    state_.running = static_cast<std::uint8_t>(operation::none);
    state_.held.clear();
    update(regs_, busy_, 0);
    update(regs_, start_, 0);
  }
  interrupt();
}

void flash::hold_hsi16(bool held)
{
  for(auto* const peer : ctx_.peers.all())
  {
    if(auto* const source = dynamic_cast<hsi16_source*>(peer))
    {
      source->hold_hsi16(held);
    }
  }
}

void flash::note_unmodelled_protection()
{
  if(not std::exchange(protection_logged_, true))
  {
    warn("FLASH_NSSR", "write protection is not modelled: WRPERR is never set");
  }
}

void flash::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != operation_end or payload != state_.generation or not busy())
  {
    return;
  }
  finish();
}

auto flash::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const done = dispatch_model::on_write(regs, offset, size, old, value, ctx);
  if(std::exchange(rejected_, false))
  {
    return std::unexpected{bus_error::denied};
  }
  return done;
}

auto flash::on_memory_write(
    region_desc const& region,
    address            addr,
    std::uint8_t       size,
    std::uint32_t      value,
    access_ctx) -> std::expected<void, bus_error>
{
  auto const at = region.physical
                    ? static_cast<std::uint32_t>(*region.physical + (addr - region.start))
                    : static_cast<std::uint32_t>(addr);
  if(busy())
  {
    state_.held.push_back({.addr = at, .size = size, .value = value});
    return {};
  }
  store(at, size, value);
  return {};
}

void flash::store(std::uint32_t addr, std::uint8_t size, std::uint32_t value)
{
  if(get(regs_, program_) == 0)
  {
    fail(sequence_error_);
    return;
  }
  if(state_.filled == 0 and errors_pending())
  {
    fail(sequence_error_);
    return;
  }
  if(size != 4)
  {
    fail(size_error_);
    return;
  }
  if(state_.filled == 0)
  {
    if(addr % quad_word != 0)
    {
      fail(alignment_error_);
      return;
    }
    state_.buffer_addr = addr;
    update(regs_, waiting_, 1);
  }
  else if(addr < state_.buffer_addr or addr >= state_.buffer_addr + quad_word)
  {
    fail(alignment_error_);
    return;
  }
  auto const word     = (addr - state_.buffer_addr) / 4;
  state_.buffer[word] = value;
  state_.filled |= static_cast<std::uint8_t>(1u << word);
  if(state_.filled != all_words)
  {
    return;
  }
  if(get(regs_, burst_) != 0 and not std::exchange(burst_logged_, true))
  {
    warn("FLASH_NSCR", "BWR burst programming is not modelled: it programs quad-word by quad-word");
  }
  update(regs_, waiting_, 0);
  start(operation::program, state_.buffer_addr, quad_word);
}

void flash::start(operation which, std::uint32_t target, std::uint32_t length)
{
  note_unmodelled_protection();
  state_.running = static_cast<std::uint8_t>(which);
  state_.target  = target;
  state_.length  = length;
  update(regs_, busy_, 1);
  auto const delay = [&] {
    switch(which)
    {
      case operation::program:
        return options_.quad_word_program;
      case operation::page_erase:
        return options_.page_erase;
      case operation::bank_erase:
        return options_.bank_erase;
      case operation::mass_erase:
      case operation::none:
        break;
    }
    return options_.mass_erase;
  }();
  schedule_in(delay, operation_end, ++state_.generation);
}

void flash::start_erase(std::uint32_t control)
{
  if(storage_ == nullptr)
  {
    warn("FLASH_NSCR", "erase requested, but no flash region names FLASH as its controller");
    return;
  }
  auto const bank_size = storage_->size / 2;
  auto const base      = static_cast<std::uint32_t>(storage_->start);
  if(page_erase_.of(control) != 0)
  {
    auto const pages = bank_size / page_size;
    auto const page  = page_.of(control);
    if(page >= pages)
    {
      update(regs_, start_, 0);
      if(not std::exchange(page_logged_, true))
      {
        warn(
            "FLASH_NSCR",
            std::format("PNB {} is past the {} pages of a bank: nothing is erased", page, pages));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, "FLASH_NSCR", reg("FLASH_NSCR").offset);
      }
      return;
    }
    start(
        operation::page_erase, base + bank_.of(control) * bank_size + page * page_size, page_size);
    return;
  }
  auto const first = bank1_erase_.of(control) != 0;
  auto const both  = first and bank2_erase_.of(control) != 0;
  start(
      both ? operation::mass_erase : operation::bank_erase, first ? base : base + bank_size,
      both ? storage_->size : bank_size);
}

void flash::program()
{
  std::array<std::byte, quad_word> target{};
  std::array<std::byte, quad_word> data{};
  for(std::size_t i = 0; i < data.size(); ++i)
  {
    data[i] = static_cast<std::byte>(state_.buffer[i / 4] >> (8 * (i % 4)));
  }
  state_.filled = 0;
  if(not ctx_.services.read_block(state_.target, target, initiator::debugger))
  {
    fail(program_error_);
    return;
  }
  auto const zero_data = all_of_byte(data, std::byte{0});
  if(all_of_byte(target, std::byte{0})
     or (not all_of_byte(target, std::byte{0xff}) and not zero_data))
  {
    fail(program_error_);
    return;
  }
  static_cast<void>(ctx_.services.write_block(state_.target, data, initiator::debugger));
}

void flash::finish()
{
  auto const which = static_cast<operation>(state_.running);
  state_.running   = static_cast<std::uint8_t>(operation::none);
  update(regs_, busy_, 0);
  auto const errors = errors_pending();
  if(which == operation::program)
  {
    program();
  }
  else
  {
    std::vector<std::byte> const erased(state_.length, std::byte{0xff});
    static_cast<void>(ctx_.services.write_block(state_.target, erased, initiator::debugger));
    update(regs_, start_, 0);
    if(get(regs_, program_) == 0)
    {
      hold_hsi16(false);
    }
  }
  if(errors_pending() == errors and get(regs_, end_irq_) != 0)
  {
    update(regs_, end_, 1);
  }
  interrupt();
  auto held = std::exchange(state_.held, {});
  for(auto const& one : held)
  {
    if(busy())
    {
      state_.held.push_back(one);
      continue;
    }
    store(one.addr, one.size, one.value);
  }
}

void flash::key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(key_.offset, 0);
  auto const locked = get(regs_, lock_) != 0;
  if(not state_.locked_up and locked and not state_.first_key and now == key1)
  {
    state_.first_key = true;
    return;
  }
  if(not state_.locked_up and locked and state_.first_key and now == key2)
  {
    state_.first_key = false;
    update(regs_, lock_, 0);
    return;
  }
  state_.first_key = false;
  state_.locked_up = true;
  rejected_        = true;
}

void flash::control_written(std::uint32_t old, std::uint32_t now)
{
  if(lock_.of(old) != 0)
  {
    regs_.poke(lock_.offset, old);
    return;
  }
  auto const erase_select = [this](std::uint32_t word) {
    return page_erase_.of(word) != 0 or bank1_erase_.of(word) != 0 or bank2_erase_.of(word) != 0;
  };
  auto const program_set  = program_.of(now) != 0 and program_.of(old) == 0;
  auto const start_set    = start_.of(now) != 0 and start_.of(old) == 0;
  auto const option_start = option_start_.of(now) != 0;
  auto const waiting      = get(regs_, waiting_) != 0;
  auto       sequence     = false;
  if(program_.of(now) != 0 and erase_select(now) and (program_set or not erase_select(old)))
  {
    sequence = true;
  }
  if(program_set and start_.of(old) != 0)
  {
    sequence = true;
  }
  if((start_set or option_start) and waiting)
  {
    sequence = true;
  }
  if(start_set and option_start)
  {
    sequence = true;
  }
  now         = option_lock_.with(now, 1);
  now         = option_start_.with(now, 0);
  now         = start_.with(now, start_.of(old));
  auto launch = false;
  if(start_set and not busy())
  {
    if(errors_pending()
       or not erase_select(now)
       or (page_erase_.of(now) != 0 and (bank1_erase_.of(now) != 0 or bank2_erase_.of(now) != 0)))
    {
      sequence = true;
    }
    if(not sequence)
    {
      now    = start_.with(now, 1);
      launch = true;
    }
  }
  regs_.poke(lock_.offset, now);
  auto const held_before = program_.of(old) != 0 or start_.of(old) != 0;
  if(sequence)
  {
    fail(sequence_error_);
  }
  if(launch)
  {
    start_erase(now);
    if(not busy())
    {
      update(regs_, start_, 0);
    }
  }
  auto const held_after = get(regs_, program_) != 0 or get(regs_, start_) != 0;
  if(held_after != held_before)
  {
    hold_hsi16(held_after);
  }
  interrupt();
}

void flash::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(status_.offset, old & ~(now & clearable_));
  interrupt();
}
}

auto flash_factory(flash_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<flash>(ctx, desc, options);
  };
}
}
