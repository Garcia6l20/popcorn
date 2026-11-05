#include "flash_rows.hpp"
#include "memory_alias.hpp"
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
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
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
constexpr std::uint32_t sector_bytes    = 4096;
constexpr std::uint32_t option_words    = 32;
constexpr std::uint32_t second_options  = 0x80;
constexpr std::uint32_t mass_erase_cost = 3;
constexpr std::uint32_t level_0         = 0xaa;
constexpr std::uint32_t level_2         = 0xcc;
constexpr std::uint32_t boot_bank_1     = 1u << 7;
constexpr auto          tprog           = std::chrono::nanoseconds{3'280'000};

struct flash_store
{
  std::uint32_t addr{};
  std::uint8_t  size{};
  std::uint32_t value{};

  friend auto operator==(flash_store const&, flash_store const&) -> bool = default;
};

struct queued_run
{
  std::uint32_t tprogs{};
  std::uint32_t count{};

  friend auto operator==(queued_run const&, queued_run const&) -> bool = default;
};

auto second_bank_of(std::span<region_desc const* const> program) -> std::optional<std::uint32_t>
{
  if(program.size() == 1)
  {
    return static_cast<std::uint32_t>(program[0]->start + program[0]->size / 2);
  }
  if(program.size() == 2)
  {
    return static_cast<std::uint32_t>(program[1]->start);
  }
  return std::nullopt;
}

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
  std::vector<queued_run>      queued{};
  std::vector<std::uint32_t>   loaded{};
  std::uint32_t                first_half{};
  std::uint32_t                second_half{};
  std::optional<std::uint32_t> erase_first{};
  std::uint8_t                 locked{};
  std::uint32_t                lock_ends{};
  bool                         options_invalid{};
  bool                         user_invalid{};

  friend auto operator==(flash_l1_state const&, flash_l1_state const&) -> bool = default;
};

/** @brief A region the bank filter covers, with the start of its bank 2 part, if any. */
struct bank_window
{
  address                      start{};
  std::uint32_t                size{};
  std::optional<std::uint32_t> second{};
};

/**
 * @brief The memory filter of the STM32L1 FLASH interface, installed by its model: while a bank
 *        is locked by a half page write or a parallel operation, a CPU or DMA load of it is a
 *        bus error and a fetch from it a BusFault, RM0038 3.4.3 note 2 and Table 15 note 3. A
 *        CPU, DMA or filtered debugger load of a PCROP sector reads 0 and calls the read
 *        protection hook, a fetch from it runs, RM0038 3.7.4. Stores reach the FLASH model,
 *        unfiltered debugger accesses and every other address pass. An access through a memory
 *        alias, the main flash at 0 of RM0038 2.7, follows the rules of the address it reaches,
 *        inferred.
 */
class bank_filter final : public bus_firewall
{
public:
  bank_filter(
      std::vector<bank_window>          windows,
      std::vector<memory_alias>         aliases,
      std::uint8_t const&               locked,
      std::vector<memory_extent> const& protected_reads,
      std::move_only_function<void()>   read_protected)
      : windows_{std::move(windows)}, aliases_{std::move(aliases)}, locked_{locked},
        protected_reads_{protected_reads}, read_protected_{std::move(read_protected)}
  {}

  [[nodiscard]] auto check_peripheral(std::size_t, bus_attrs, arch::memory_op)
      -> bus_verdict override
  {
    return bus_verdict::allow;
  }

  [[nodiscard]] auto filters_memory() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto
      memory_page(address seen_page, std::uint32_t size, address seen, bus_attrs, arch::memory_op)
          -> page_verdict override
  {
    auto const page        = physical_of(aliases_, seen_page);
    auto const bank_locked = locked(physical_of(aliases_, seen));
    auto const read_ruled  = std::ranges::any_of(protected_reads_, [&](memory_extent const& e) {
      return e.first < std::uint64_t{page} + size and e.last >= page;
    });
    return {.filter_data = bank_locked or read_ruled, .deny_fetch = bank_locked};
  }

  [[nodiscard]] auto check_memory(address seen, std::uint8_t, bus_attrs attrs, arch::memory_op op)
      -> bus_verdict override
  {
    auto const at = physical_of(aliases_, seen);
    if(op == arch::memory_op::write)
    {
      return bus_verdict::allow;
    }
    if(attrs.by != initiator::debugger and locked(at))
    {
      return bus_verdict::bus_error;
    }
    if(read_protected(at))
    {
      read_protected_();
      return bus_verdict::read_zero_write_ignore;
    }
    return bus_verdict::allow;
  }

  [[nodiscard]] auto allowed_extent(address seen, std::uint8_t, bus_attrs, arch::memory_op op)
      -> memory_extent override
  {
    auto const* const alias  = alias_at(aliases_, seen);
    auto const        at     = physical_of(aliases_, seen);
    auto              extent = bank_extent(at);
    if(op != arch::memory_op::write)
    {
      for(auto const& area : protected_reads_)
      {
        extent = outside(extent, at, area);
      }
    }
    if(alias != nullptr)
    {
      return through(*alias, extent);
    }
    for(auto const& other : aliases_)
    {
      extent = outside(
          extent, at,
          {.first = other.start, .last = static_cast<address>(other.start + (other.size - 1u))});
    }
    return extent;
  }

private:
  [[nodiscard]] static auto outside(memory_extent extent, address at, memory_extent area)
      -> memory_extent
  {
    if(area.last < at)
    {
      extent.first = std::max(extent.first, area.last + 1);
    }
    else if(area.first > at)
    {
      extent.last = std::min(extent.last, area.first - 1);
    }
    return extent;
  }

  [[nodiscard]] auto read_protected(address at) const -> bool
  {
    return std::ranges::any_of(
        protected_reads_, [at](memory_extent const& e) { return at >= e.first and at <= e.last; });
  }

  [[nodiscard]] auto bank_extent(address at) const -> memory_extent
  {
    if(locked_ == 0)
    {
      return {.first = 0, .last = ~address{}};
    }
    std::uint64_t first = 0;
    std::uint64_t last  = ~address{};
    for(auto const& [start, size, second] : windows_)
    {
      auto const end = std::uint64_t{start} + size;
      if(at >= start and at < end)
      {
        if(second and at >= *second)
        {
          return {.first = *second, .last = static_cast<address>(end - 1)};
        }
        auto const top = second and *second < end ? std::uint64_t{*second} : end;
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

  [[nodiscard]] auto locked(address at) const -> bool
  {
    if(locked_ == 0)
    {
      return false;
    }
    auto const found = std::ranges::find_if(
        windows_, [at](bank_window const& w) { return at >= w.start and at - w.start < w.size; });
    if(found == windows_.end())
    {
      return false;
    }
    auto const bank = found->second and at >= *found->second ? 2u : 1u;
    return (locked_ & bank) != 0;
  }

  std::vector<bank_window>          windows_;
  std::vector<memory_alias>         aliases_;
  std::uint8_t const&               locked_;
  std::vector<memory_extent> const& protected_reads_;
  std::move_only_function<void()>   read_protected_;
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
  field_ref read_error;

  [[nodiscard]] auto errors() const -> std::uint32_t
  {
    return protection_error.mask()
         | alignment_error.mask()
         | size_error.mask()
         | option_error.mask()
         | user_option_error.mask()
         | read_error.mask();
  }
};

/**
 * @brief FLASH and data EEPROM interface of the STM32L1, RM0038 section 3, the category
 *        differences from the variant.
 *
 *        Locks, RM0038 3.4.1: FLASH_PECR.PELOCK, PRGLOCK and OPTLOCK are set after every reset.
 *        PEKEY1 then PEKEY2 in FLASH_PEKEYR clears PELOCK, PRGKEY1 then PRGKEY2 in FLASH_PRGKEYR
 *        clears PRGLOCK, OPTKEY1 then OPTKEY2 in FLASH_OPTKEYR clears OPTLOCK. A wrong key, a third
 *        write, or a write to FLASH_PRGKEYR or FLASH_OPTKEYR while PELOCK is set is a bus error
 *        that sets the lock it targets, which then stays set until the next reset. The key
 *        registers read 0. While PELOCK is set a FLASH_PECR write changes nothing. The lock bits
 *        are set by a write of 1 only, and setting PELOCK sets PRGLOCK and OPTLOCK and clears PROG,
 *        DATA, FTDW, ERASE and FPRG. PROG, DATA, FTDW, ERASE, FPRG and PARALLELBANK keep their
 *        value while BSY is set, PARALLELBANK reads 0 on a part with one bank. Setting OBL_LAUNCH
 *        while OPTLOCK is clear requests an option byte loader reset, reset_kind::option_load.
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
 *        an operation it completes adds its tprogs to BSY, EOP setting at the end of each tprog.
 *        The core runs on during BSY, except that from the first store of a half page sequence to
 *        the end of its tprog a CPU or DMA load of the bank written, its program memory and its
 *        half of the data EEPROM, is a bus error and a fetch from it a BusFault, both banks for a
 *        parallel operation, RM0038 3.4.3 note 2 and Table 15 note 3, through the model's bus
 *        firewall. A data EEPROM double word locks its bank the same way. While such an operation
 *        is BSY a program memory or data EEPROM store to the other bank sets WRPERR and writes
 *        nothing, RM0038 3.4.4 and 3.7.3. A store to a write protected sector, 16 pages per bit
 *        of FLASH_WRPRx counted over the Flash regions in address order, sets WRPERR, RM0038
 *        3.7.2.
 *
 *        Data EEPROM, RM0038 3.4.2, 3.4.3 and Table 15: with PELOCK clear, a byte, halfword or
 *        word store writes as given. A word store of 0 at a word boundary is a word erase of one
 *        tprog. Any other store takes one tprog per word it touches, two when FTDW is set or the
 *        word did not read 0. With ERASE and DATA set, two word stores from an 8-byte boundary
 *        erase a double word, with FPRG and DATA set they program it, one tprog. A first store
 *        off the boundary or a second store not at the next word sets PGAERR, a byte or
 *        halfword store SIZERR. On Cat.1 a byte or halfword store of 0 programs nothing, logs a
 *        model_warning once and raises model_error under strict_peripherals, Table 20 note 7. A
 *        store while PELOCK is set sets WRPERR.
 *
 *        Option bytes, RM0038 3.5, 3.5.1 and 3.7.1: with PELOCK and OPTLOCK clear, a word store to
 *        the option words of OPTION_FACTORY, offsets 0x00 to 0x1F and on a part with two banks
 *        0x80 to 0x9F, writes as given, one or two tprog as the data EEPROM. A byte or halfword
 *        store sets SIZERR, a store elsewhere in the region, to the factory information, or any
 *        store at RDP level 2 sets WRPERR. At RDP level 1 a store of RDP 0xAA to offset 0 first
 *        erases to 0 the Flash regions, the data EEPROM, the option words and the RTC backup
 *        registers, and takes three tprog. A power-on, brown-out or option load reset loads
 *        FLASH_OBR and FLASH_WRPRx from the option words: RDPRT from RDP, bits 23 to 16 from USER,
 *        WRPRx from WRPx. A byte or halfword whose complement is wrong loads 0 and sets OPTVERR,
 *        and OPTVERRUSR as well for USER on Cat.3 to Cat.6. FLASH_OBR and FLASH_WRPRx ignore
 *        writes. RDPRT 0xAA is level 0, 0xCC level 2, any other value level 1.
 *
 *        PCROP, RM0038 3.7.4, on the `pcrop` variant, the L151, L152 and L162 Cat.2 and Cat.3 on
 *        DIE427: SPRMOD loads with RDP as one pair against nSPRMOD and nRDP. With SPRMOD set a
 *        FLASH_WRPRx bit at 0 protects its sector: a program or erase of it sets WRPERR, and a
 *        CPU, DMA or filtered debugger load of it reads 0 and sets FLASH_SR.RDERR, a fetch from
 *        it runs, inferred. An option store while SPRMOD is set programs with SPRMOD and every
 *        protected WRPx bit kept. The RDP level 1 to 0 store programs the SPRMOD bit it
 *        carries, and its mass erase clears the WRPx words.
 *
 *        A machine built with host option bytes on a part with two banks loads nBFB2,
 *        FLASH_OBR bit 23, as the inverse of `swap_bank` at every load, whatever the USER word
 *        holds.
 *
 *        FLASH_SR: EOP, WRPERR, PGAERR, SIZERR, OPTVERR and OPTVERRUSR clear on a write of 1. EOP
 *        sets once at the end of each operation, after all of its tprogs. FLASH_IRQn is raised
 *        while EOP and EOPIE, or an error flag and ERRIE, are set, RM0038 3.8. Every reset, and an
 *        RCC FLITFRST pulse while FLASH_ACR.RUN_PD is set, RM0038 6.3.5, bring the registers to
 *        their description reset with READY set, set every lock and end the operation in flight
 *        and the queued ones. A pulse with RUN_PD clear changes nothing. FLASH_OBR and FLASH_WRPRx
 *        keep their loaded value at every other reset and at every pulse. OPTVERR and OPTVERRUSR
 *        set again at every reset and pulse after a load that set them, RM0038 3.9.7, the
 *        write-1 clear lasting until then, inferred.
 */
class flash_l1 final : public dispatch_model<flash_l1, flash_l1_state>,
                       public clock_listener,
                       public voltage_range_listener,
                       public bus_reset_listener
{
public:
  flash_l1(
      model_context&          ctx,
      peripheral_desc const&  desc,
      flash_l1_variant const& variant,
      flash_options const&    options);

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
  void option_register_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto range() const -> std::uint32_t;
  void               restore(reset_kind kind);
  void               lock(std::uint32_t& control);
  void               program(flash_store const& word);
  void               program_data(flash_store const& word);
  void               program_option(flash_store const& word);
  void               double_word(std::uint32_t addr, std::uint32_t value);
  void               write_timed(flash_store const& word);
  void               mass_erase();
  void               load_options();
  void               note_range(std::uint32_t addr);
  [[nodiscard]] auto rdp_level() const -> std::uint32_t;
  [[nodiscard]] auto pcrop_mode() const -> bool;
  [[nodiscard]] auto sector_of(std::uint32_t addr) const -> std::optional<std::uint32_t>;
  [[nodiscard]] auto sector_bit(std::uint32_t sector) const -> bool;
  [[nodiscard]] auto kept_option(std::uint32_t offset, std::uint32_t value) const -> std::uint32_t;
  void               update_protected_reads();
  void               read_refused();
  [[nodiscard]] auto write_protected(std::uint32_t addr) const -> bool;
  [[nodiscard]] auto read_word(std::uint32_t addr) const -> std::uint32_t;
  [[nodiscard]] auto option_word(std::uint32_t offset) const -> bool;
  [[nodiscard]] auto data_bank(region_desc const& region) const -> std::optional<std::uint32_t>;
  [[nodiscard]] static auto loads_options(reset_kind kind) -> bool;
  void                      erase_page(std::uint32_t addr);
  void                      load_half_page(std::uint32_t addr, std::uint32_t value);
  void                      start(std::span<std::byte const> bytes, std::uint32_t at);
  void                      abort_sequence();
  void                      lock_banks(std::uint8_t banks);
  void                      release_banks();
  [[nodiscard]] auto        bank_of(std::uint32_t addr) const -> std::uint8_t;
  void                      begin_locked();
  [[nodiscard]] auto        in_second_bank(std::uint32_t addr) const -> bool;
  [[nodiscard]] auto        parallel() const -> bool;
  [[nodiscard]] auto        other_bank_busy(std::uint32_t addr) const -> bool;
  void                      begin(std::uint32_t tprogs = 1);
  void                      fail(field_ref flag);
  void                      finish();
  void                      interrupts();
  void                      check_latency(std::string_view trigger);
  void                      note_once(bool& logged, std::string_view reg, std::string message);
  auto                      key_step(
      std::uint8_t& keys,
      bool&         locked_up,
      bool          armed,
      field_ref     lock,
      std::uint32_t key1,
      std::uint32_t key2,
      std::uint32_t now) -> bool;

  register_file&                                                    regs_;
  flash_l1_variant                                                  variant_;
  field_ref                                                         latency_;
  field_ref                                                         prefetch_;
  field_ref                                                         wide_;
  field_ref                                                         sleep_down_;
  field_ref                                                         run_down_;
  control_fields                                                    control_;
  status_fields                                                     status_;
  std::array<reg_ref, 4>                                            keys_{};
  reg_ref                                                           options_;
  field_ref                                                         rdp_;
  field_ref                                                         sprmod_;
  std::vector<memory_extent>                                        protected_reads_;
  std::vector<reg_ref>                                              protection_;
  std::vector<region_desc const*>                                   program_regions_;
  std::vector<region_desc const*>                                   data_regions_;
  region_desc const*                                                option_region_{};
  std::optional<std::uint32_t>                                      second_bank_;
  std::optional<bool>                                               boot_bank_2_;
  std::unique_ptr<bank_filter>                                      filter_;
  irq_line&                                                         line_;
  core::clock_domain                                                hclk_;
  std::uint32_t                                                     written_{};
  std::uint32_t                                                     written_offset_{};
  bool                                                              rejected_{};
  bool                                                              mode_logged_{};
  bool                                                              range_logged_{};
  bool                                                              null_logged_{};
  std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> latency_logged_;
};

constexpr std::array<write_entry<flash_l1>, 5> flash_l1_option_writes{
    {
     {"OBR", &flash_l1::option_register_written},
     {"WRPR1", &flash_l1::option_register_written},
     {"WRPR2", &flash_l1::option_register_written},
     {"WRPR3", &flash_l1::option_register_written},
     {"WRPR4", &flash_l1::option_register_written},
     }
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

flash_l1::flash_l1(
    model_context&          ctx,
    peripheral_desc const&  desc,
    flash_l1_variant const& variant,
    flash_options const&    options)
    : dispatch_model{ctx, desc, "FLASH", flash_l1_writes}, regs_{ctx.regs}, variant_{variant},
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
          .user_option_error = field("SR", "OPTVERRUSR"),
          .read_error        = variant.pcrop ? field("SR", "RDERR") : field_ref{}},
      keys_{reg("PDKEYR"), reg("PEKEYR"), reg("PRGKEYR"), reg("OPTKEYR")}, options_{reg("OBR")},
      rdp_{field("OBR", "RDPRT")}, sprmod_{variant.pcrop ? field("OBR", "SPRMOD") : field_ref{}},
      line_{irq("FLASH_IRQn")}, hclk_{ctx.clock.domain("hclk")}
{
  for(std::uint8_t index = 1; index <= 4; ++index)
  {
    expect_register(std::format("WRPR{}", index), index <= variant.wrp_registers);
  }
  bind(std::span{flash_l1_option_writes}.first(1u + variant.wrp_registers));
  for(auto const& entry : std::span{flash_l1_option_writes}.subspan(1, variant.wrp_registers))
  {
    protection_.push_back(reg(entry.reg));
  }
  for(auto const& region : ctx.part.regions)
  {
    if(region.controller != desc.name or region.physical)
    {
      continue;
    }
    if(region.acc.execute)
    {
      program_regions_.push_back(&region);
    }
    else if(region.name == "OPTION_FACTORY")
    {
      option_region_ = &region;
    }
    else
    {
      data_regions_.push_back(&region);
    }
  }
  if(option_region_ == nullptr)
  {
    throw config_error(
        ctx.part.part, 0, "regions", "an OPTION_FACTORY region that names FLASH as its controller");
  }
  std::ranges::sort(program_regions_, {}, &region_desc::start);
  second_bank_ = variant.dual_bank ? second_bank_of(program_regions_) : std::nullopt;
  if(variant.dual_bank and not second_bank_)
  {
    throw config_error(
        ctx.part.part, 0, "regions",
        "one or two Flash regions that name FLASH as their controller, the two banks");
  }
  if(options.option_bytes)
  {
    auto const encoded = l1_encode_options(
        variant, {.unit = &desc, .regions = program_regions_}, *options.option_bytes);
    if(not encoded)
    {
      throw config_error(desc.name, 0, encoded.error().key, encoded.error().expected);
    }
    if(variant.dual_bank)
    {
      boot_bank_2_ = options.option_bytes->swap_bank;
    }
  }
  std::vector<bank_window> windows;
  for(auto const* const region : program_regions_)
  {
    windows.push_back({.start = region->start, .size = region->size, .second = second_bank_});
  }
  for(auto const* const region : data_regions_)
  {
    windows.push_back({.start = region->start, .size = region->size, .second = data_bank(*region)});
  }
  filter_ = std::make_unique<bank_filter>(
      std::move(windows), memory_aliases(ctx.part), state_.locked, protected_reads_,
      [this] { read_refused(); });
  ctx.install_firewall(*filter_);
}

auto flash_l1::data_bank(region_desc const& region) const -> std::optional<std::uint32_t>
{
  if(not variant_.dual_bank)
  {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(region.start + region.size / 2);
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
  auto const ends  = busy() ? std::ranges::fold_left(
                                  state_.queued, 2u,
                                  [](std::uint32_t sum, queued_run run) { return sum + run.count; })
                            : 1u;
  state_.lock_ends = std::max(state_.lock_ends, ends);
  begin();
}

auto flash_l1::in_second_bank(std::uint32_t addr) const -> bool
{
  for(auto const* const region : data_regions_)
  {
    if(addr >= region->start and addr - region->start < region->size)
    {
      auto const second = data_bank(*region);
      return second and addr >= *second;
    }
  }
  return second_bank_ and addr >= *second_bank_;
}

auto flash_l1::other_bank_busy(std::uint32_t addr) const -> bool
{
  return busy() and state_.lock_ends != 0 and (state_.locked & bank_of(addr)) == 0;
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

auto flash_l1::loads_options(reset_kind kind) -> bool
{
  return kind == reset_kind::power_on
      or kind == reset_kind::brown_out
      or kind == reset_kind::option_load;
}

void flash_l1::restore(reset_kind kind)
{
  for(auto const& entry : flash_l1_writes)
  {
    regs_.poke(reg(entry.reg).offset, describe(entry.reg).reset);
  }
  update(regs_, status_.ready, 1);
  if(loads_options(kind))
  {
    load_options();
    update_protected_reads();
  }
  auto const generation   = state_.generation;
  auto const locked       = state_.locked;
  auto const invalid      = state_.options_invalid;
  auto const user_invalid = state_.user_invalid;
  state_                  = {};
  state_.generation       = generation;
  state_.options_invalid  = invalid;
  state_.user_invalid     = user_invalid;
  if(invalid)
  {
    update(regs_, status_.option_error, 1);
  }
  if(user_invalid)
  {
    update(regs_, status_.user_option_error, 1);
  }
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
  protected_reads_.clear();
  update_protected_reads();
  ctx_.services.memory_policy_changed();
  latency_logged_.clear();
  mode_logged_  = false;
  range_logged_ = false;
  null_logged_  = false;
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
  if(not state_.queued.empty())
  {
    auto const tprogs = state_.queued.front().tprogs;
    if(--state_.queued.front().count == 0)
    {
      state_.queued.erase(state_.queued.begin());
    }
    schedule_in(tprog * tprogs, operation_end, ++state_.generation);
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
     or other_bank_busy(word.addr))
  {
    fail(status_.protection_error);
    return;
  }
  if(write_protected(word.addr))
  {
    abort_sequence();
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
  note_range(word.addr);
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

void flash_l1::note_range(std::uint32_t addr)
{
  if(range() == lowest_range)
  {
    note_once(
        range_logged_, "PECR",
        std::format(
            "a program of 0x{:08x} in voltage range 3, RM0038 3.6 allows programming in ranges 1 "
            "and 2 only",
            addr));
  }
}

void flash_l1::begin(std::uint32_t tprogs)
{
  if(busy())
  {
    if(state_.queued.empty() or state_.queued.back().tprogs != tprogs)
    {
      state_.queued.push_back({.tprogs = tprogs});
    }
    ++state_.queued.back().count;
    return;
  }
  update(regs_, status_.busy, 1);
  update(regs_, status_.high_voltage_end, 0);
  schedule_in(tprog * tprogs, operation_end, ++state_.generation);
}

auto flash_l1::read_word(std::uint32_t addr) const -> std::uint32_t
{
  std::array<std::byte, 4> bytes{};
  static_cast<void>(ctx_.services.read_block(addr, bytes, initiator::debugger));
  return std::bit_cast<std::uint32_t>(bytes);
}

auto flash_l1::sector_of(std::uint32_t addr) const -> std::optional<std::uint32_t>
{
  std::uint32_t before = 0;
  for(auto const* const region : program_regions_)
  {
    if(addr >= region->start and addr - region->start < region->size)
    {
      return (before + static_cast<std::uint32_t>(addr - region->start)) / sector_bytes;
    }
    before += region->size;
  }
  return std::nullopt;
}

auto flash_l1::sector_bit(std::uint32_t sector) const -> bool
{
  auto const index = sector / 32;
  return index < protection_.size()
     and (regs_.peek(protection_[index].offset) >> (sector % 32) & 1u) != 0;
}

auto flash_l1::pcrop_mode() const -> bool
{
  return variant_.pcrop and get(regs_, sprmod_) != 0;
}

auto flash_l1::write_protected(std::uint32_t addr) const -> bool
{
  auto const sector = sector_of(addr);
  if(not sector or *sector / 32 >= protection_.size())
  {
    return false;
  }
  return sector_bit(*sector) != pcrop_mode();
}

void flash_l1::update_protected_reads()
{
  std::vector<memory_extent> extents;
  if(pcrop_mode())
  {
    std::uint32_t before = 0;
    for(auto const* const region : program_regions_)
    {
      for(std::uint32_t at = 0; at < region->size; at += sector_bytes)
      {
        auto const sector = (before + at) / sector_bytes;
        if(sector / 32 >= protection_.size() or sector_bit(sector))
        {
          continue;
        }
        auto const first = static_cast<address>(region->start + at);
        auto const last  = static_cast<address>(first + sector_bytes - 1);
        if(not extents.empty() and extents.back().last + 1 == first)
        {
          extents.back().last = last;
        }
        else
        {
          extents.push_back({.first = first, .last = last});
        }
      }
      before += region->size;
    }
  }
  if(extents != protected_reads_)
  {
    protected_reads_ = std::move(extents);
    ctx_.services.memory_policy_changed();
  }
}

void flash_l1::read_refused()
{
  update(regs_, status_.read_error, 1);
  interrupts();
}

auto flash_l1::kept_option(std::uint32_t offset, std::uint32_t value) const -> std::uint32_t
{
  if(not pcrop_mode())
  {
    return value;
  }
  if(offset == 0)
  {
    return (value | sprmod_.mask()) & ~(sprmod_.mask() << 16);
  }
  if(offset < 8 or offset >= 8 + 8 * protection_.size())
  {
    return value;
  }
  auto const index    = (offset - 8) / 8;
  auto const shift    = (offset - 8) % 8 == 0 ? 0u : 16u;
  auto const loaded   = regs_.peek(protection_[index].offset) >> shift & 0xffffu;
  auto const kept_low = ~loaded & 0xffffu;
  return (value & ~kept_low) | (kept_low << 16);
}

auto flash_l1::rdp_level() const -> std::uint32_t
{
  auto const rdp = get(regs_, rdp_);
  return rdp == level_0 ? 0 : rdp == level_2 ? 2 : 1;
}

auto flash_l1::option_word(std::uint32_t offset) const -> bool
{
  return offset < option_words
      or (variant_.dual_bank
          and offset >= second_options
          and offset < second_options + option_words);
}

void flash_l1::write_timed(flash_store const& word)
{
  auto const    fixed  = get(regs_, control_.fixed_time) != 0;
  std::uint32_t tprogs = 0;
  if(word.size == 4 and word.addr % 4 == 0 and word.value == 0)
  {
    tprogs = 1;
  }
  else
  {
    for(auto at = word.addr & ~3u; at < word.addr + word.size; at += 4)
    {
      tprogs += fixed or read_word(at) != 0 ? 2 : 1;
    }
  }
  auto const bytes = std::bit_cast<std::array<std::byte, 4>>(word.value);
  start(std::span{bytes}.first(word.size), word.addr);
  begin(tprogs);
}

void flash_l1::double_word(std::uint32_t addr, std::uint32_t value)
{
  if(state_.loaded.empty())
  {
    if(addr % 8 != 0)
    {
      fail(status_.alignment_error);
      return;
    }
    state_.first_half = addr;
    state_.loaded.push_back(value);
    lock_banks(bank_of(addr));
    return;
  }
  if(addr != state_.first_half + 4)
  {
    abort_sequence();
    fail(status_.alignment_error);
    return;
  }
  std::array<std::uint32_t, 2> const words{state_.loaded.front(), value};
  state_.loaded.clear();
  start(std::as_bytes(std::span{words}), state_.first_half);
  begin_locked();
}

void flash_l1::program_data(flash_store const& word)
{
  auto const control = regs_.peek(control_.pe_lock.offset);
  if(control_.pe_lock.of(control) != 0 or other_bank_busy(word.addr))
  {
    fail(status_.protection_error);
    return;
  }
  auto const erase     = control_.erase.of(control) != 0;
  auto const half_page = control_.half_page.of(control) != 0;
  if(erase or half_page)
  {
    if(erase == half_page or control_.data.of(control) == 0 or control_.program.of(control) != 0)
    {
      note_once(
          mode_logged_, "PECR",
          std::format(
              "a data EEPROM store to 0x{:08x} with ERASE or FPRG set and DATA clear or PROG "
              "set, or with both set: nothing is programmed",
              word.addr));
      return;
    }
    if(word.size != 4)
    {
      abort_sequence();
      fail(status_.size_error);
      return;
    }
    note_range(word.addr);
    double_word(word.addr, erase ? 0 : word.value);
    return;
  }
  auto const mask = word.size == 4 ? ~std::uint32_t{} : (std::uint32_t{1} << (8 * word.size)) - 1;
  if(word.size < 4 and (word.value & mask) == 0 and not variant_.null_partial_writes)
  {
    note_once(
        null_logged_, "PECR",
        std::format(
            "a {}-byte store of 0 to the data EEPROM at 0x{:08x}, RM0038 Table 20 note 7 allows "
            "only a non-zero byte or halfword on Cat.1: nothing is programmed",
            word.size, word.addr));
    return;
  }
  note_range(word.addr);
  write_timed(word);
}

void flash_l1::program_option(flash_store const& word)
{
  auto const control = regs_.peek(control_.pe_lock.offset);
  if(control_.pe_lock.of(control) != 0 or control_.option_lock.of(control) != 0)
  {
    fail(status_.protection_error);
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
  auto const offset = static_cast<std::uint32_t>(word.addr - option_region_->start);
  if(not option_word(offset) or rdp_level() == 2)
  {
    fail(status_.protection_error);
    return;
  }
  if(control_.erase.of(control) != 0 or control_.half_page.of(control) != 0)
  {
    note_once(
        mode_logged_, "PECR",
        std::format(
            "an option byte store to 0x{:08x} with ERASE or FPRG set: nothing is programmed, "
            "RM0038 3.5.1 allows word writes and word erases only",
            word.addr));
    return;
  }
  note_range(word.addr);
  if(offset == 0 and rdp_level() == 1 and (word.value & 0xffu) == level_0)
  {
    mass_erase();
    start(std::bit_cast<std::array<std::byte, 4>>(word.value), word.addr);
    begin(mass_erase_cost);
    return;
  }
  write_timed({.addr = word.addr, .size = word.size, .value = kept_option(offset, word.value)});
}

void flash_l1::mass_erase()
{
  std::uint32_t largest = option_words;
  for(auto const* const region : program_regions_)
  {
    largest = std::max(largest, region->size);
  }
  for(auto const* const region : data_regions_)
  {
    largest = std::max(largest, region->size);
  }
  std::vector<std::byte> const zeros(largest);
  auto const                   erase = [&](address at, std::uint32_t size) {
    start(std::span{zeros}.first(size), static_cast<std::uint32_t>(at));
  };
  for(auto const* const region : program_regions_)
  {
    erase(region->start, region->size);
  }
  for(auto const* const region : data_regions_)
  {
    erase(region->start, region->size);
  }
  erase(option_region_->start, option_words);
  if(variant_.dual_bank)
  {
    erase(option_region_->start + second_options, option_words);
  }
  auto const rtc =
      std::ranges::find(ctx_.part.peripherals, std::string_view{"RTC"}, &peripheral_desc::name);
  if(rtc == ctx_.part.peripherals.end())
  {
    return;
  }
  for(auto const& backup : rtc->registers)
  {
    if(backup.name.starts_with("BKP"))
    {
      static_cast<void>(ctx_.services.write_block(
          rtc->base + backup.offset, std::span{zeros}.first(4), initiator::debugger));
    }
  }
}

void flash_l1::load_options()
{
  auto const base = static_cast<std::uint32_t>(option_region_->start);
  auto const pair = [this, base](std::uint32_t offset, std::uint32_t bits) {
    auto const word = read_word(base + offset);
    auto const mask = (std::uint32_t{1} << bits) - 1;
    auto const low  = word & mask;
    return ((word >> 16) & mask) == (~low & mask) ? std::optional{low} : std::nullopt;
  };
  auto const rdp    = pair(0, variant_.pcrop ? 9 : 8);
  auto const user   = pair(4, 8);
  auto       failed = not rdp or not user;
  auto const boot   = boot_bank_2_.transform([](bool bank_2) { return bank_2 ? 0 : boot_bank_1; });
  auto const loaded = boot ? (user.value_or(0) & ~boot_bank_1) | *boot : user.value_or(0);
  regs_.poke(options_.offset, (loaded << 16) | rdp.value_or(0));
  for(std::uint32_t index = 0; index < protection_.size(); ++index)
  {
    auto const offset = index < 3 ? 8 + 8 * index : second_options;
    auto const low    = pair(offset, 16);
    auto const high   = pair(offset + 4, 16);
    failed            = failed or not low or not high;
    regs_.poke(protection_[index].offset, (high.value_or(0) << 16) | low.value_or(0));
  }
  state_.options_invalid = failed;
  state_.user_invalid    = not user and variant_.user_option_flag;
}

auto flash_l1::on_memory_write(
    region_desc const& region,
    address            addr,
    std::uint8_t       size,
    std::uint32_t      value,
    access_ctx) -> std::expected<void, bus_error>
{
  flash_store const word{.addr = static_cast<std::uint32_t>(addr), .size = size, .value = value};
  if(region.acc.execute)
  {
    program(word);
  }
  else if(region.start == option_region_->start)
  {
    program_option(word);
  }
  else
  {
    program_data(word);
  }
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
  written_offset_ = offset & ~3u;
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
        control_.parallel_bank.with(word, variant_.dual_bank ? control_.parallel_bank.of(now) : 0u);
    auto const modes = control_.program.mask()
                     | control_.data.mask()
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
  if(control_.option_lock.of(old) == 0 and control_.option_launch.of(word) != 0)
  {
    ctx_.services.request_reset(reset_kind::option_load);
  }
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

void flash_l1::option_register_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(written_offset_, old);
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
  auto const limits  = variant_.max_khz[level];
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

auto flash_l1_factory(flash_options const& options, flash_l1_variant variant) -> model_factory
{
  return [variant,
          options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<flash_l1>(ctx, desc, variant, options);
  };
}

auto l1_encode_options(
    flash_l1_variant const& variant,
    flash_program const&    program,
    option_bytes const&     wanted) -> std::expected<address, option_refusal>
{
  auto const refuse = [](std::string_view field, std::string expected) {
    return std::unexpected{
        option_refusal{
                       .key = std::format("option_bytes.{}", field), .expected = std::move(expected)}
    };
  };
  auto const unset = [](std::string_view what) {
    return std::format("unset on an STM32L1, RM0038 has no {}", what);
  };
  if(wanted.tzen)
  {
    return refuse("tzen", unset("TrustZone"));
  }
  if(not std::holds_alternative<whole_bank>(wanted.secwm1))
  {
    return refuse("secwm1", unset("secure watermark"));
  }
  if(not std::holds_alternative<whole_bank>(wanted.secwm2))
  {
    return refuse("secwm2", unset("secure watermark"));
  }
  if(wanted.hdp1)
  {
    return refuse("hdp1", unset("hide protection"));
  }
  if(wanted.hdp2)
  {
    return refuse("hdp2", unset("hide protection"));
  }
  if(wanted.secboot)
  {
    return refuse("secboot", unset("boot address option byte"));
  }
  if(wanted.nsboot)
  {
    return refuse("nsboot", unset("boot address option byte"));
  }
  if(wanted.dualbank)
  {
    return refuse("dualbank", unset("bank organization option, the part fixes it"));
  }
  auto const base = program.regions.front()->start;
  if(not wanted.swap_bank)
  {
    return base;
  }
  auto const second = variant.dual_bank ? second_bank_of(program.regions) : std::nullopt;
  if(not second)
  {
    return refuse(
        "swap_bank",
        "false on an STM32L1 with one bank, nBFB2 exists on Cat.4, Cat.5 and Cat.6 only, RM0038 "
        "Table 19");
  }
  return address{*second};
}
}
