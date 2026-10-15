#include "factories.hpp"
#include "hsi16_source.hpp"
#include "model_base.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/types.hpp>

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
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t key1          = 0x4567'0123;
constexpr std::uint32_t key2          = 0xcdef'89ab;
constexpr std::uint32_t quad_word     = 16;
constexpr std::uint32_t burst_size    = 8 * quad_word;
constexpr std::uint32_t burst_words   = burst_size / 4;
constexpr std::uint32_t page_size     = 8 * 1024;
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
  std::uint32_t control{};
  bool          dma{};

  friend auto operator==(held_store const&, held_store const&) -> bool = default;
};

struct flash_state
{
  bool                                   first_key{};
  bool                                   locked_up{};
  std::array<std::uint32_t, burst_words> buffer{};
  std::uint32_t                          filled{};
  bool                                   burst{};
  std::uint32_t                          buffer_addr{};
  std::uint8_t                           running{};
  std::uint32_t                          target{};
  std::uint32_t                          length{};
  std::uint32_t                          generation{};
  std::vector<held_store>                held{};
  std::uint64_t                          due_ps{};
  std::vector<std::uint32_t>             torn{};

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

constexpr std::uint64_t mhz = 1'000'000;

constexpr std::array<std::array<std::uint64_t, 5>, 4> normal_read_max_mhz{
    {
     {32, 64, 96, 128, 160},
     {30, 60, 90, 110, 0},
     {24, 48, 55, 0, 0},
     {12, 25, 0, 0, 0},
     }
};

constexpr std::array<std::uint64_t, 3> low_power_range4_max_mhz{8, 16, 25};

auto first_wait_states(std::span<std::uint64_t const> max_mhz, std::uint64_t hclk)
    -> std::optional<std::uint32_t>
{
  for(std::uint32_t ws = 0; ws < max_mhz.size() and max_mhz[ws] != 0; ++ws)
  {
    if(hclk <= max_mhz[ws] * mhz)
    {
      return ws;
    }
  }
  return std::nullopt;
}

/**
 * @brief Wait states RM0456 7.3.3 asks for, Table 54 with LPM clear, Table 55 with LPM set.
 * @param hclk HCLK in Hz.
 * @param range Voltage range, 1 to 4.
 * @param low_power FLASH_ACR.LPM.
 * @return The least LATENCY, empty when HCLK is above the maximum of the range.
 */
auto required_wait_states(std::uint64_t hclk, std::uint32_t range, bool low_power)
    -> std::optional<std::uint32_t>
{
  if(low_power and range == 4)
  {
    return first_wait_states(low_power_range4_max_mhz, hclk);
  }
  auto const normal = first_wait_states(normal_read_max_mhz[range - 1], hclk);
  if(not low_power or not normal)
  {
    return normal;
  }
  auto const tens   = (hclk + 10 * mhz - 1) / (10 * mhz);
  auto const needed = static_cast<std::uint32_t>(tens > 0 ? tens - 1 : 0);
  return std::max(*normal, needed);
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
 * flash_options::quad_word_program, then the 16 bytes are written. With BWR also set when the
 * first word is stored, the buffer collects a burst of 8 quad-words: the first word at a 128-byte
 * address, the 31 others at consecutive addresses, then one program of the 128 bytes holds BSY
 * for flash_options::burst_program. RM0456 leaves a BWR change while the buffer fills
 * undefined: the mode of the first word holds until the buffer launches or is flushed. Erase,
 * RM0456 7.3.6: STRT with PER erases the page PNB of the bank BKER, with MER1 and MER2 one or both
 * banks, BSY set for the flash_options time, then the range reads 0xFF and STRT clears. EOP is set
 * at the end only with EOPIE. Geometry: option bytes are not modelled, so the model uses the
 * production FLASH_OPTR value 0x1FEFF8AA, RM0456 7.9.13: DUALBANK = 1, SWAP_BANK = 0, bank 2 at the
 * flash base plus half its size, 8 Kbyte pages. A PNB at or above the pages of a bank, which RM0456
 * leaves undefined, erases nothing, sets no flag and logs a model_warning once, and raises
 * model_error under strict_peripherals.
 *
 * Errors, RM0456 7.3.9: PGSERR, SIZERR, PGAERR and PROGERR as its list states. For PROGERR the
 * 7.3.9 list wins over the FLASH_NSSR bit text, which accepts all-zero data over an all-zero
 * target: the model refuses it. A burst is checked per quad-word and programs nothing when one
 * of its 8 quad-words fails. Every error aborts the operation and flushes the write buffer.
 * With ERRIE an error also sets OPERR. FLASH_IRQn follows EOP with EOPIE and OPERR with ERRIE.
 * WRPERR is not modelled: write protection needs option bytes, logged once. HSI16 is kept on
 * while PG or STRT is set, through the RCC model.
 *
 * Latency, RM0456 7.3.3: FLASH_ACR.LATENCY is compared with Table 54, or Table 55 with LPM set,
 * for HCLK and the voltage range PWR_SVMSR.ACTVOS applies, on a FLASH_ACR write, an HCLK change
 * and a range change. Silicon refuses nothing and reads wrong data with too few wait states, so
 * a violation logs a model_warning once per HCLK, range and LATENCY since power-on or the last
 * restore, naming its trigger. Under strict_peripherals every violation logs it and raises
 * model_error, which a violation found in an RCC or PWR event carries out of the run. Without a
 * PWR model the range is taken as range 1, the most permissive, so no violation is invented. A
 * correct RM0456 10.5.4 sequence never violates the table at those points.
 *
 * Deviations: a store into flash while BSY is set stalls the bus on silicon until BSY clears.
 * The model lets the store complete at once and applies it by the rules above when BSY clears,
 * with the PG and BWR bits FLASH_NSCR held when the store was made, as the stalled bus would
 * have seen them. A store from a DMA channel instead takes the PG and BWR bits FLASH_NSCR holds
 * when BSY clears, since silicon stalls the channel and not the CPU, which can still write
 * FLASH_NSCR meanwhile. The next instructions run earlier than on silicon. The held stores have no
 * bound: a store stream during a 195 ms bank erase keeps every word until the erase ends. Code
 * fetches from a bank being erased or programmed also stall on silicon, RM0456 7.3.10, the model
 * keeps executing without a stall. STRT written while BSY is set, which RM0456 leaves undefined, is
 * ignored: STRT stays set and no flag is raised.
 *
 * Power cut: RM0456 7.3.11 leaves the contents of a program or erase cut in flight undefined. The
 * model applies a deterministic stand-in that depends only on the virtual time of the cut, not
 * a behaviour of silicon. Of the `n` quad-words of the operation, the first
 * `floor(n * elapsed / duration)` are done. For a program the next quad-word is torn: its first
 * 8 data bytes are ANDed into the old bytes, its last 8 keep theirs, the rest keep their old
 * bytes. A program that would set PROGERR at its end changes nothing. For an erase the done
 * quad-words read 0xFF, the next one keeps its old bytes and is torn unless they are all 0xFF,
 * the rest keep theirs. Torn quad-words are kept across every reset and in the snapshot, and only
 * an erase that covers one clears it. A program into a range holding a torn quad-word sets
 * PROGERR, RM0456 7.3.12. A partly filled write buffer is dropped. A reset of any kind outside
 * `power_cycle()`, a power-on reset or one a model requests included, still drops the operation
 * in flight.
 */
class flash final : public dispatch_model<flash, flash_state>,
                    public clock_listener,
                    public voltage_range_listener
{
public:
  flash(model_context& ctx, peripheral_desc const& desc, flash_options const& options);

  void reset(register_file& regs, reset_kind kind) override;
  void power_cut() override;
  void load(std::span<std::byte const> in) override;
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

  void on_clock_change() override;
  void on_voltage_range_change() override;

  void access_written(std::uint32_t old, std::uint32_t now);
  void key_written(std::uint32_t old, std::uint32_t now);
  void control_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto errors_pending() const -> bool;
  void               fail(field_ref flag);
  void               interrupt();
  void               finish();
  void               store(held_store const& one);
  void               start(operation which, std::uint32_t target, std::uint32_t length);
  void               start_erase(std::uint32_t control);
  [[nodiscard]] auto duration_of(operation which, std::uint32_t length) const
      -> std::chrono::nanoseconds;
  [[nodiscard]] auto refused(
      std::uint32_t              at,
      std::span<std::byte const> target,
      std::span<std::byte const> data) const -> bool;
  [[nodiscard]] auto buffered() const -> std::array<std::byte, burst_size>;
  void               tear_program(std::uint32_t full);
  void               tear_erase(std::uint32_t full);
  void               forget_torn(std::uint32_t from, std::uint32_t length);
  void               program();
  void               hold_hsi16(bool held);
  void               note_unmodelled_protection();
  void               check_latency(std::string_view trigger);

  flash_options                                                     options_;
  register_file&                                                    regs_;
  region_desc const*                                                storage_{};
  reg_ref                                                           key_;
  reg_ref                                                           status_;
  field_ref                                                         lock_;
  field_ref                                                         option_lock_;
  field_ref                                                         start_;
  field_ref                                                         option_start_;
  field_ref                                                         program_;
  field_ref                                                         page_erase_;
  field_ref                                                         bank1_erase_;
  field_ref                                                         bank2_erase_;
  field_ref                                                         page_;
  field_ref                                                         bank_;
  field_ref                                                         burst_;
  field_ref                                                         end_irq_;
  field_ref                                                         error_irq_;
  field_ref                                                         end_;
  field_ref                                                         operation_error_;
  field_ref                                                         program_error_;
  field_ref                                                         protection_error_;
  field_ref                                                         alignment_error_;
  field_ref                                                         size_error_;
  field_ref                                                         sequence_error_;
  field_ref                                                         option_error_;
  field_ref                                                         busy_;
  field_ref                                                         waiting_;
  irq_line&                                                         line_;
  field_ref                                                         latency_;
  field_ref                                                         low_power_read_;
  core::clock_domain                                                hclk_;
  std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> latency_logged_;
  std::uint32_t                                                     clearable_{};
  bool                                                              rejected_{};
  bool                                                              page_logged_{};
  bool                                                              protection_logged_{};
};

constexpr std::array<write_entry<flash>, 4> flash_writes{
    {
     {"FLASH_ACR", &flash::access_written},
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
      line_{irq("FLASH_IRQn")}, latency_{field("FLASH_ACR", "LATENCY")},
      low_power_read_{field("FLASH_ACR", "LPM")}, hclk_{ctx.clock.domain("hclk")}
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
  auto       torn       = std::move(state_.torn);
  state_                = {};
  state_.generation     = generation + 1;
  state_.torn           = std::move(torn);
  line_.lower();
}

void flash::power_cut()
{
  auto const which = static_cast<operation>(state_.running);
  if(which == operation::none)
  {
    return;
  }
  state_.running   = static_cast<std::uint8_t>(operation::none);
  auto const now   = ctx_.clock.since_power_on().ps;
  auto const total = static_cast<std::uint64_t>(duration_of(which, state_.length).count()) * 1000u;
  auto const left  = state_.due_ps > now ? state_.due_ps - now : 0;
  auto const lines = state_.length / quad_word;
  auto const full =
      total == 0
          ? lines
          : static_cast<std::uint32_t>(
                static_cast<unsigned __int128>(lines) * (total - std::min(total, left)) / total);
  if(which == operation::program)
  {
    tear_program(full);
  }
  else
  {
    tear_erase(full);
  }
}

auto flash::duration_of(operation which, std::uint32_t length) const -> std::chrono::nanoseconds
{
  switch(which)
  {
    case operation::program:
      return length == quad_word ? options_.quad_word_program : options_.burst_program;
    case operation::page_erase:
      return options_.page_erase;
    case operation::bank_erase:
      return options_.bank_erase;
    case operation::mass_erase:
    case operation::none:
      break;
  }
  return options_.mass_erase;
}

auto flash::refused(
    std::uint32_t              at,
    std::span<std::byte const> target,
    std::span<std::byte const> data) const -> bool
{
  for(std::size_t line = 0; line < data.size(); line += quad_word)
  {
    auto const old       = target.subspan(line, quad_word);
    auto const zero_data = all_of_byte(data.subspan(line, quad_word), std::byte{0});
    if(all_of_byte(old, std::byte{0}) or (not all_of_byte(old, std::byte{0xff}) and not zero_data))
    {
      return true;
    }
  }
  auto const first = std::ranges::lower_bound(state_.torn, at);
  return first != state_.torn.end() and *first < at + data.size();
}

auto flash::buffered() const -> std::array<std::byte, burst_size>
{
  std::array<std::byte, burst_size> data{};
  for(std::size_t i = 0; i < data.size(); ++i)
  {
    data[i] = static_cast<std::byte>(state_.buffer[i / 4] >> (8 * (i % 4)));
  }
  return data;
}

void flash::tear_program(std::uint32_t full)
{
  std::array<std::byte, burst_size> target_bytes{};
  auto const                        target     = std::span{target_bytes}.first(state_.length);
  auto                              data_bytes = buffered();
  auto const                        data       = std::span{data_bytes}.first(state_.length);
  if(not ctx_.services.read_block(state_.target, target, initiator::debugger)
     or refused(state_.target, target, data))
  {
    return;
  }
  auto const done = full * quad_word;
  if(done < data.size())
  {
    for(std::size_t i = done; i < done + quad_word / 2; ++i)
    {
      data[i] &= target[i];
    }
    std::ranges::copy(target.subspan(done + quad_word / 2), data.begin() + done + quad_word / 2);
    auto const line = state_.target + done;
    state_.torn.insert(std::ranges::upper_bound(state_.torn, line), line);
  }
  static_cast<void>(ctx_.services.write_block(state_.target, data, initiator::debugger));
}

void flash::tear_erase(std::uint32_t full)
{
  auto const                   done = full * quad_word;
  std::vector<std::byte> const erased(done, std::byte{0xff});
  static_cast<void>(ctx_.services.write_block(state_.target, erased, initiator::debugger));
  forget_torn(state_.target, done);
  if(done >= state_.length)
  {
    return;
  }
  std::array<std::byte, quad_word> old{};
  auto const                       line = state_.target + done;
  if(not ctx_.services.read_block(line, old, initiator::debugger)
     or all_of_byte(old, std::byte{0xff}))
  {
    return;
  }
  auto const at = std::ranges::lower_bound(state_.torn, line);
  if(at == state_.torn.end() or *at != line)
  {
    state_.torn.insert(at, line);
  }
}

void flash::forget_torn(std::uint32_t from, std::uint32_t length)
{
  auto const first = std::ranges::lower_bound(state_.torn, from);
  auto const last  = std::ranges::lower_bound(state_.torn, from + length);
  state_.torn.erase(first, last);
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
    access_ctx         ctx) -> std::expected<void, bus_error>
{
  auto const       at = region.physical
                          ? static_cast<std::uint32_t>(*region.physical + (addr - region.start))
                          : static_cast<std::uint32_t>(addr);
  held_store const one{
      .addr    = at,
      .size    = size,
      .value   = value,
      .control = regs_.peek(program_.offset),
      .dma     = ctx.by == initiator::dma};
  if(busy())
  {
    state_.held.push_back(one);
    return {};
  }
  store(one);
  return {};
}

void flash::store(held_store const& one)
{
  auto const [addr, size, value, written, dma] = one;
  auto const control                           = dma ? regs_.peek(program_.offset) : written;
  if(program_.of(control) == 0)
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
  auto const next =
      state_.buffer_addr + 4 * static_cast<std::uint32_t>(std::popcount(state_.filled));
  if(state_.filled == 0)
  {
    state_.burst = burst_.of(control) != 0;
    if(addr % (state_.burst ? burst_size : quad_word) != 0)
    {
      fail(alignment_error_);
      return;
    }
    state_.buffer_addr = addr;
    update(regs_, waiting_, 1);
  }
  else if(
      state_.burst ? addr != next
                   : addr < state_.buffer_addr or addr >= state_.buffer_addr + quad_word)
  {
    fail(alignment_error_);
    return;
  }
  auto const length   = state_.burst ? burst_size : quad_word;
  auto const word     = (addr - state_.buffer_addr) / 4;
  state_.buffer[word] = value;
  state_.filled |= 1u << word;
  if(std::cmp_not_equal(std::popcount(state_.filled), length / 4))
  {
    return;
  }
  update(regs_, waiting_, 0);
  start(operation::program, state_.buffer_addr, length);
}

void flash::start(operation which, std::uint32_t target, std::uint32_t length)
{
  note_unmodelled_protection();
  state_.running = static_cast<std::uint8_t>(which);
  state_.target  = target;
  state_.length  = length;
  update(regs_, busy_, 1);
  auto const delay = duration_of(which, length);
  state_.due_ps =
      ctx_.clock.since_power_on().ps + static_cast<std::uint64_t>(delay.count()) * 1000u;
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
  std::array<std::byte, burst_size> target_bytes{};
  auto const                        target     = std::span{target_bytes}.first(state_.length);
  auto const                        data_bytes = buffered();
  auto const                        data       = std::span{data_bytes}.first(state_.length);
  state_.filled                                = 0;
  if(not ctx_.services.read_block(state_.target, target, initiator::debugger)
     or refused(state_.target, target, data))
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
    forget_torn(state_.target, state_.length);
    update(regs_, start_, 0);
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
    store(one);
  }
  if(not busy() and get(regs_, program_) == 0 and get(regs_, start_) == 0)
  {
    hold_hsi16(false);
  }
}

void flash::check_latency(std::string_view trigger)
{
  auto const hclk = ctx_.clock.hz(hclk_);
  if(hclk == 0)
  {
    return;
  }
  std::uint32_t range = 1;
  for(auto* const peer : ctx_.peers.all())
  {
    if(auto const* const source = dynamic_cast<voltage_range_source const*>(peer))
    {
      range = source->active_range();
    }
  }
  auto const latency  = get(regs_, latency_);
  auto const required = required_wait_states(hclk, range, get(regs_, low_power_read_) != 0);
  if(required and latency >= *required)
  {
    return;
  }
  if(latency_logged_.emplace(hclk, range, latency).second or ctx_.strict_peripherals)
  {
    warn(
        "FLASH_ACR",
        required ? std::format(
                       "{}: LATENCY {} is below the {} wait states RM0456 7.3.3 asks for HCLK {} "
                       "Hz in voltage range {}, silicon would read wrong data",
                       trigger, latency, *required, hclk, range)
                 : std::format(
                       "{}: HCLK {} Hz is above the maximum of voltage range {}, RM0456 7.3.3",
                       trigger, hclk, range));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, "FLASH_ACR", reg("FLASH_ACR").offset);
  }
}

void flash::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  latency_logged_.clear();
}

void flash::on_clock_change()
{
  check_latency("after an HCLK change");
}

void flash::on_voltage_range_change()
{
  check_latency("after a voltage range change");
}

void flash::access_written(std::uint32_t, std::uint32_t)
{
  check_latency("on a FLASH_ACR write");
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
  auto const held_after = get(regs_, program_) != 0 or get(regs_, start_) != 0 or busy();
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
