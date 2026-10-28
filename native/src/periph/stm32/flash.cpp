#include "factories.hpp"
#include "family_names.hpp"
#include "gtzc.hpp"
#include "hsi16_source.hpp"
#include "model_base.hpp"
#include "temporal_isolation.hpp"
#include "voltage_range.hpp"

#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
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
constexpr std::uint32_t key1          = 0x4567'0123;
constexpr std::uint32_t key2          = 0xcdef'89ab;
constexpr std::uint32_t option_key1   = 0x0819'2a3b;
constexpr std::uint32_t option_key2   = 0x4c5d'6e7f;
constexpr std::uint32_t buffer_words  = 32;
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
  bool                                    first_key{};
  bool                                    locked_up{};
  bool                                    option_first_key{};
  bool                                    option_locked_up{};
  std::array<std::uint32_t, buffer_words> buffer{};
  std::uint32_t                           filled{};
  bool                                    burst{};
  std::uint32_t                           buffer_addr{};
  std::uint8_t                            running{};
  std::uint32_t                           target{};
  std::uint32_t                           length{};
  std::uint32_t                           generation{};
  std::vector<held_store>                 held{};
  std::uint64_t                           due_ps{};
  std::vector<std::uint32_t>              torn{};
  std::vector<std::uint32_t>              wrp{};
  std::vector<std::uint32_t>              wrp_active{};

  friend auto operator==(flash_state const&, flash_state const&) -> bool = default;
};

constexpr std::uint8_t held_secure     = 1;
constexpr std::uint8_t held_privileged = 2;

struct secure_flash_state
{
  bool                      first_key{};
  bool                      locked_up{};
  bool                      buffer_secure{};
  bool                      running_secure{};
  std::vector<std::uint8_t> held_attrs{};

  friend auto operator==(secure_flash_state const&, secure_flash_state const&) -> bool = default;
};

struct trustzone_flash_state
{
  flash_state        flash{};
  secure_flash_state secure{};

  friend auto operator==(trustzone_flash_state const&, trustzone_flash_state const&)
      -> bool = default;
};

enum class set_role : std::uint8_t
{
  lock,
  program,
  start,
  end,
  end_irq,
  busy,
  buffer_open,
  page_erase,
  page,
  bank_select,
  bank1_erase,
  bank2_erase,
  burst,
  sector_erase,
  sector,
  bank_erase,
  mass_erase,
  force_write,
  error_irq,
  operation_error,
  program_error,
  protection_error,
  alignment_error,
  size_error,
  sequence_error,
  strobe_error,
  inconsistency_error,
  protection_irq,
  sequence_irq,
  strobe_irq,
  inconsistency_irq,
};

constexpr std::array<named_field<set_role>, 31> u5_set_fields{
    {
     {set_role::lock, {"CR", "LOCK"}},
     {set_role::program, {"CR", "PG"}},
     {set_role::start, {"CR", "STRT"}},
     {set_role::end, {"SR", "EOP"}},
     {set_role::end_irq, {"CR", "EOPIE"}},
     {set_role::busy, {"SR", "BSY"}},
     {set_role::buffer_open, {"SR", "WDW"}},
     {set_role::page_erase, {"CR", "PER"}},
     {set_role::page, {"CR", "PNB"}},
     {set_role::bank_select, {"CR", "BKER"}},
     {set_role::bank1_erase, {"CR", "MER1"}},
     {set_role::bank2_erase, {"CR", "MER2"}},
     {set_role::burst, {"CR", "BWR"}},
     {set_role::sector_erase, {}},
     {set_role::sector, {}},
     {set_role::bank_erase, {}},
     {set_role::mass_erase, {}},
     {set_role::force_write, {}},
     {set_role::error_irq, {"CR", "ERRIE"}},
     {set_role::operation_error, {"SR", "OPERR"}},
     {set_role::program_error, {"SR", "PROGERR"}},
     {set_role::protection_error, {"SR", "WRPERR"}},
     {set_role::alignment_error, {"SR", "PGAERR"}},
     {set_role::size_error, {"SR", "SIZERR"}},
     {set_role::sequence_error, {"SR", "PGSERR"}},
     {set_role::strobe_error, {}},
     {set_role::inconsistency_error, {}},
     {set_role::protection_irq, {}},
     {set_role::sequence_irq, {}},
     {set_role::strobe_irq, {}},
     {set_role::inconsistency_irq, {}},
     }
};

constexpr std::array<named_field<set_role>, 31> h5_set_fields{
    {
     {set_role::lock, {"CR", "LOCK"}},
     {set_role::program, {"CR", "PG"}},
     {set_role::start, {"CR", "STRT"}},
     {set_role::end, {"SR", "EOP"}},
     {set_role::end_irq, {"CR", "EOPIE"}},
     {set_role::busy, {"SR", "BSY"}},
     {set_role::buffer_open, {"SR", "WBNE"}},
     {set_role::page_erase, {}},
     {set_role::page, {}},
     {set_role::bank_select, {"CR", "BKSEL"}},
     {set_role::bank1_erase, {}},
     {set_role::bank2_erase, {}},
     {set_role::burst, {}},
     {set_role::sector_erase, {"CR", "SER"}},
     {set_role::sector, {"CR", "SNB"}},
     {set_role::bank_erase, {"CR", "BER"}},
     {set_role::mass_erase, {"CR", "MER"}},
     {set_role::force_write, {"CR", "FW"}},
     {set_role::error_irq, {}},
     {set_role::operation_error, {}},
     {set_role::program_error, {}},
     {set_role::protection_error, {"SR", "WRPERR"}},
     {set_role::alignment_error, {}},
     {set_role::size_error, {}},
     {set_role::sequence_error, {"SR", "PGSERR"}},
     {set_role::strobe_error, {"SR", "STRBERR"}},
     {set_role::inconsistency_error, {"SR", "INCERR"}},
     {set_role::protection_irq, {"CR", "WRPERRIE"}},
     {set_role::sequence_irq, {"CR", "PGSERRIE"}},
     {set_role::strobe_irq, {"CR", "STRBERRIE"}},
     {set_role::inconsistency_irq, {"CR", "INCERRIE"}},
     }
};

enum class unit_role : std::uint8_t
{
  latency,
  programming_delay,
  low_power_read,
  ecc_detect,
  ecc_correct,
  ecc_bank,
  ecc_address,
  invert,
  hide_bank1,
  hide_bank2,
  hide_extension1,
  hide_extension2,
  option_lock,
  option_start,
  option_load,
  option_error,
  option_error_irq,
  option_swap,
};

constexpr std::array<named_field<unit_role>, 18> u5_unit_fields{
    {
     {unit_role::latency, {"FLASH_ACR", "LATENCY"}},
     {unit_role::programming_delay, {}},
     {unit_role::low_power_read, {"FLASH_ACR", "LPM"}},
     {unit_role::ecc_detect, {"FLASH_ECCR", "ECCD"}},
     {unit_role::ecc_correct, {"FLASH_ECCR", "ECCC"}},
     {unit_role::ecc_bank, {"FLASH_ECCR", "BK_ECC"}},
     {unit_role::ecc_address, {"FLASH_ECCR", "ADDR_ECC"}},
     {unit_role::invert, {"FLASH_SECCR", "INV"}},
     {unit_role::hide_bank1, {"FLASH_SECHDPCR", "HDP1_ACCDIS"}},
     {unit_role::hide_bank2, {"FLASH_SECHDPCR", "HDP2_ACCDIS"}},
     {unit_role::hide_extension1, {}},
     {unit_role::hide_extension2, {}},
     {unit_role::option_lock, {"FLASH_NSCR", "OPTLOCK"}},
     {unit_role::option_start, {"FLASH_NSCR", "OPTSTRT"}},
     {unit_role::option_load, {"FLASH_NSCR", "OBL_LAUNCH"}},
     {unit_role::option_error, {"FLASH_NSSR", "OPTWERR"}},
     {unit_role::option_error_irq, {}},
     {unit_role::option_swap, {}},
     }
};

constexpr std::array<named_field<unit_role>, 18> h5_unit_fields{
    {
     {unit_role::latency, {"FLASH_ACR", "LATENCY"}},
     {unit_role::programming_delay, {"FLASH_ACR", "WRHIGHFREQ"}},
     {unit_role::low_power_read, {}},
     {unit_role::ecc_detect, {"FLASH_ECCDETR", "ECCD"}},
     {unit_role::ecc_correct, {"FLASH_ECCCORR", "ECCC"}},
     {unit_role::ecc_bank, {"FLASH_ECCDETR", "BK_ECC"}},
     {unit_role::ecc_address, {"FLASH_ECCDETR", "ADDR_ECC"}},
     {unit_role::invert, {"FLASH_SECCR", "INV"}},
     {unit_role::hide_bank1, {}},
     {unit_role::hide_bank2, {}},
     {unit_role::hide_extension1, {"FLASH_HDPEXTR", "HDP1_EXT"}},
     {unit_role::hide_extension2, {"FLASH_HDPEXTR", "HDP2_EXT"}},
     {unit_role::option_lock, {"FLASH_OPTCR", "OPTLOCK"}},
     {unit_role::option_start, {"FLASH_OPTCR", "OPTSTRT"}},
     {unit_role::option_load, {}},
     {unit_role::option_error, {"FLASH_NSSR", "OPTCHANGEERR"}},
     {unit_role::option_error_irq, {"FLASH_NSCR", "OPTCHANGEERRIE"}},
     {unit_role::option_swap, {"FLASH_OPTCR", "SWAP_BANK"}},
     }
};

enum class privilege_role : std::uint8_t
{
  nonsecure,
  secure,
};

constexpr std::array<named_field<privilege_role>, 2> u5_privilege_fields{
    {
     {privilege_role::nonsecure, {"FLASH_PRIVCFGR", "NSPRIV"}},
     {privilege_role::secure, {"FLASH_PRIVCFGR", "SPRIV"}},
     }
};

constexpr std::array<set_role, 7> error_roles{
    set_role::program_error,       set_role::size_error,     set_role::alignment_error,
    set_role::protection_error,    set_role::sequence_error, set_role::strobe_error,
    set_role::inconsistency_error,
};

/** @brief An error flag and the enable that raises the FLASH interrupt while it is set. */
struct error_enable
{
  /** @brief Error flag role. */
  set_role flag{};
  /** @brief Interrupt enable role. */
  set_role enable{};
};

/**
 * @brief Register a role names in @p table, found by role, empty when the family lacks it.
 * @param table Name table of one family.
 * @param role Role to find.
 */
template <typename Role>
consteval auto register_of(std::span<named_field<Role> const> table, Role role) -> std::string_view
{
  auto const found = std::ranges::find(table, role, &named_field<Role>::role);
  return found == table.end() ? std::string_view{} : found->name.reg;
}

/**
 * @brief Whether every named role of a register set table lives in its `CR` or its `SR`.
 * @param table Set role names of one family.
 */
consteval auto names_control_or_status(std::span<named_field<set_role> const> table) -> bool
{
  return std::ranges::all_of(table, [](named_field<set_role> const& entry) {
    return entry.name.reg.empty() or entry.name.reg == "CR" or entry.name.reg == "SR";
  });
}

constexpr std::size_t pages_per_block_register = 32;

/** @brief What decides whether a page lies in a closed HDP area. */
struct hide_inputs
{
  /** @brief Watermark option word of the page's bank. */
  std::uint32_t watermark{};
  /** @brief HDP option word of the page's bank. */
  std::uint32_t area{};
  /** @brief Access disable bit of the bank, set. */
  bool access_disabled{};
  /** @brief Extension of the bank's HDP area in pages. */
  std::uint32_t extension{};
  /** @brief Temporal isolation level HDPL. */
  std::uint8_t level{};
  /** @brief Page in its bank. */
  std::uint32_t page{};
  /** @brief Mask of a page number field. */
  std::uint32_t top{};
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

auto physical(region_desc const& region, address addr) -> std::uint32_t
{
  return region.physical ? static_cast<std::uint32_t>(*region.physical + (addr - region.start))
                         : static_cast<std::uint32_t>(addr);
}

struct bank_geometry
{
  std::uint32_t base{};
  std::uint32_t size{};
};

/** @brief Encoded option registers, in the order of the traits' option_registers. */
template <std::size_t N> struct option_words
{
  /** @brief One word per option register. */
  std::array<std::uint32_t, N> words{};
  /** @brief Boot address the option bytes select. */
  address boot{};
};

/** @brief A write protection register firmware programs and the register that reads it back. */
struct wrp_names
{
  /** @brief Register OPTSTRT programs from. */
  std::string_view program;
  /** @brief Register that reads the loaded protection. */
  std::string_view active;
};

constexpr std::uint32_t wrp_unlock = 1u << 31;

constexpr std::uint32_t optr_production        = 0x1fef'f8aa;
constexpr std::uint32_t optr_tzen              = 1u << 31;
constexpr std::uint32_t optr_dualbank          = 1u << 21;
constexpr std::uint32_t optr_swap_bank         = 1u << 20;
constexpr std::uint32_t nsbootadd1_production  = 0x0bf9'007f;
constexpr std::uint32_t secure_boot_default    = 0x0c00'0000;
constexpr std::uint32_t nonsecure_boot_default = 0x0800'0000;
constexpr std::uint32_t boot_granule           = 128;
constexpr std::uint32_t nsboot_reserved        = 0x7f;
constexpr std::uint32_t secboot_reserved       = 0x7c;
constexpr std::uint32_t secwm_r1_reserved      = 0xff00'ff00;
constexpr std::uint32_t secwm_r2_reserved      = 0x7f00'7f00;
constexpr std::uint32_t hdp_enable             = 1u << 31;

auto watermark_bits(std::string_view part) -> std::uint32_t
{
  for(std::string_view const line : {"STM32U535", "STM32U545"})
  {
    if(part.starts_with(line))
    {
      return 5;
    }
  }
  for(std::string_view const line : {"STM32U59", "STM32U5A", "STM32U5F", "STM32U5G"})
  {
    if(part.starts_with(line))
    {
      return 8;
    }
  }
  return 7;
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

/** @brief Registers of one FLASH register set. */
struct set_names
{
  /** @brief Key register. */
  std::string_view key;
  /** @brief Control register, the `CR` of the set roles. */
  std::string_view control;
  /** @brief Status register, the `SR` of the set roles. */
  std::string_view status;
  /** @brief Register whose bits clear the status flags through the description's cross rules,
   * empty when the status register clears by writing 1. */
  std::string_view clear{};
};

/** @brief Erase a control register word asks for, before any range check. */
struct erase_command
{
  /** @brief Page, bank or mass erase. */
  operation op{};
  /** @brief Bank of a page or bank erase. */
  std::uint32_t bank{};
  /** @brief Page in its bank of a page erase. */
  std::uint32_t page{};
};

/** @brief STM32U5 FLASH traits, RM0456 section 7, each capability tied to its roles. */
struct u5_flash
{
  /** @brief BWR collects a burst of 8 quad-words, RM0456 7.3.7. */
  static constexpr bool burst_write = true;
  /** @brief FW forces a partly filled write buffer out. */
  static constexpr bool force_write = false;
  /** @brief PER, PNB, MER1 and MER2 select the erase, RM0456 7.3.6. */
  static constexpr bool erase_by_page = true;
  /** @brief OPERR with ERRIE summarizes every error, RM0456 7.3.9. */
  static constexpr bool summary_error = true;
  /** @brief OBL_LAUNCH requests an option byte loader reset, RM0456 7.4.2. */
  static constexpr bool option_load = true;
  /** @brief Each error flag has its own interrupt enable. */
  static constexpr bool per_error_irq = false;
  /** @brief EOP sets at the end of every operation, EOPIE clear too. */
  static constexpr bool end_flag_always = false;
  /** @brief PG or STRT keeps HSI16 on through the RCC model, RM0456 7.3.1. */
  static constexpr bool holds_hsi16 = true;
  /** @brief The option key sequence needs FLASH_NSCR.LOCK clear, RM0456 7.4.2. */
  static constexpr bool option_key_needs_unlock = true;
  /** @brief A completed OPTSTRT loads the programmed protection at once. */
  static constexpr bool option_reload_on_start = false;
  /** @brief Clearing ECCD also clears the address and bank it recorded. */
  static constexpr bool ecc_record_clears = false;
  /** @brief Control field the out-of-range page warning names. */
  static constexpr std::string_view page_field = "PNB";
  /** @brief Datasheet times of an empty flash_options field. */
  static constexpr flash_timing timing = u5_flash_timing;
  /** @brief Error flags with their own interrupt enable. */
  static constexpr std::span<error_enable const> error_enables{};
  /** @brief Bytes of one program, a quad-word, RM0456 7.3.7. */
  static constexpr std::uint32_t program_bytes = 16;
  /** @brief Bytes of the write buffer, a burst of 8 quad-words. */
  static constexpr std::uint32_t buffer_bytes = 8 * program_bytes;
  /** @brief Bytes of an erase page, RM0456 7.3.6. */
  static constexpr std::uint32_t page_bytes = 8 * 1024;
  /** @brief Section the latency warnings cite. */
  static constexpr std::string_view latency_section = "RM0456 7.3.3";
  /** @brief Register sets, the Non-secure one first, then the Secure one. */
  static constexpr std::array<set_names, 2> set_registers{
      {
       {"FLASH_NSKEYR", "FLASH_NSCR", "FLASH_NSSR"},
       {"FLASH_SECKEYR", "FLASH_SECCR", "FLASH_SECSR"},
       }
  };
  /** @brief Role names of each register set. */
  static constexpr std::span<named_field<set_role> const> set_fields = u5_set_fields;
  /** @brief Role names of the fields outside the register sets. */
  static constexpr std::span<named_field<unit_role> const> unit_fields = u5_unit_fields;
  /** @brief Role names of the TrustZone privilege fields. */
  static constexpr std::span<named_field<privilege_role> const> privilege_fields =
      u5_privilege_fields;
  /** @brief Option registers loaded from flash_options::option_bytes, in option_words order. */
  static constexpr std::array<std::string_view, 8> option_registers{
      "FLASH_OPTR",     "FLASH_NSBOOTADD0R", "FLASH_NSBOOTADD1R", "FLASH_SECBOOTADD0R",
      "FLASH_SECWM1R1", "FLASH_SECWM1R2",    "FLASH_SECWM2R1",    "FLASH_SECWM2R2",
  };
  /** @brief Option registers that read zero and ignore writes with TZEN = 0, RM0456 7.5.1. */
  static constexpr std::array<std::string_view, 5> secure_option_registers{
      "FLASH_SECBOOTADD0R", "FLASH_SECWM1R1", "FLASH_SECWM1R2", "FLASH_SECWM2R1", "FLASH_SECWM2R2",
  };
  /** @brief Registers firmware writes for OPTSTRT besides option_registers. */
  static constexpr std::array<std::string_view, 0> option_staging_registers{};
  /** @brief Write protection registers, bank 1 area A and B, then bank 2. */
  static constexpr std::array<wrp_names, 4> wrp_registers{
      {
       {"FLASH_WRP1AR", "FLASH_WRP1AR"},
       {"FLASH_WRP1BR", "FLASH_WRP1BR"},
       {"FLASH_WRP2AR", "FLASH_WRP2AR"},
       {"FLASH_WRP2BR", "FLASH_WRP2BR"},
       }
  };
  /** @brief Registers the dropped-option warning names as the ones OPTSTRT programs. */
  static constexpr std::string_view wrp_text = "FLASH_WRPxyR";
  /** @brief Block-based security registers of bank 1, then bank 2, RM0456 7.9.30. */
  static constexpr std::array<std::string_view, 16> secure_block_registers{
      "FLASH_SEC1BBR1", "FLASH_SEC1BBR2", "FLASH_SEC1BBR3", "FLASH_SEC1BBR4",
      "FLASH_SEC1BBR5", "FLASH_SEC1BBR6", "FLASH_SEC1BBR7", "FLASH_SEC1BBR8",
      "FLASH_SEC2BBR1", "FLASH_SEC2BBR2", "FLASH_SEC2BBR3", "FLASH_SEC2BBR4",
      "FLASH_SEC2BBR5", "FLASH_SEC2BBR6", "FLASH_SEC2BBR7", "FLASH_SEC2BBR8",
  };
  /** @brief Block-based privilege registers of bank 1, then bank 2, RM0456 7.9.32. */
  static constexpr std::array<std::string_view, 16> privileged_block_registers{
      "FLASH_PRIV1BBR1", "FLASH_PRIV1BBR2", "FLASH_PRIV1BBR3", "FLASH_PRIV1BBR4",
      "FLASH_PRIV1BBR5", "FLASH_PRIV1BBR6", "FLASH_PRIV1BBR7", "FLASH_PRIV1BBR8",
      "FLASH_PRIV2BBR1", "FLASH_PRIV2BBR2", "FLASH_PRIV2BBR3", "FLASH_PRIV2BBR4",
      "FLASH_PRIV2BBR5", "FLASH_PRIV2BBR6", "FLASH_PRIV2BBR7", "FLASH_PRIV2BBR8",
  };
  /** @brief Block registers of one bank, 32 pages each. */
  static constexpr std::size_t block_registers_per_bank = 8;
  /** @brief Index in option_registers of the watermark word of bank 1 and bank 2. */
  static constexpr std::array<std::size_t, 2> watermark_words{4, 6};
  /** @brief Index in option_registers of the HDP word of bank 1 and bank 2. */
  static constexpr std::array<std::size_t, 2> hide_words{5, 7};

  /**
   * @brief Whether a page lies in the closed HDP area of its bank, RM0456 7.5.3: HDPxEN and
   *        HDPx_ACCDIS set, from SECWMx_PSTRT to HDPx_PEND.
   * @param in Option words, bits and page of the bank.
   */
  [[nodiscard]] static constexpr auto hidden(hide_inputs const& in) -> bool
  {
    auto const start = in.watermark & in.top;
    return (in.area & hdp_enable) != 0
       and in.access_disabled
       and start <= in.page
       and in.page <= (in.area >> 16 & in.top);
  }

  /**
   * @brief Highest page number of a SECWM, HDP or WRP field of @p part, RM0456 7.9.17.
   * @param part Part name.
   */
  [[nodiscard]] static auto watermark_top(std::string_view part) -> std::uint32_t
  {
    return (1u << watermark_bits(part)) - 1;
  }

  /**
   * @brief Bits of an option register a firmware write is compared on, the fields of the part.
   * @param name Register name.
   * @param part Part name.
   */
  [[nodiscard]] static auto option_mask(std::string_view name, std::string_view part)
      -> std::uint32_t
  {
    auto const top = watermark_top(part);
    if(name == "FLASH_SECWM1R1" or name == "FLASH_SECWM2R1")
    {
      return top | top << 16;
    }
    if(name == "FLASH_SECWM1R2" or name == "FLASH_SECWM2R2")
    {
      return hdp_enable | top << 16;
    }
    if(name == "FLASH_NSBOOTADD0R" or name == "FLASH_NSBOOTADD1R")
    {
      return ~nsboot_reserved;
    }
    if(name == "FLASH_SECBOOTADD0R")
    {
      return ~secboot_reserved;
    }
    return ~0u;
  }

  /**
   * @brief Production value of a write protection register: PSTRT at the field maximum, PEND 0.
   * @param part Part name.
   */
  [[nodiscard]] static auto wrp_production(std::string_view part) -> std::uint32_t
  {
    return ~(watermark_top(part) << 16);
  }

  /**
   * @brief A written write protection word as the register stores it, other bits read 1.
   * @param word Written word.
   * @param part Part name.
   */
  [[nodiscard]] static auto wrp_normal(std::uint32_t word, std::string_view part) -> std::uint32_t
  {
    auto const top    = watermark_top(part);
    auto const fields = wrp_unlock | top << 16 | top;
    return (word & fields) | ~fields;
  }

  /**
   * @brief Whether OPTSTRT refuses a change: a programmed word with UNLOCK 0 cannot change.
   * @param programmed Programmed word.
   * @param requested Requested word.
   */
  [[nodiscard]] static constexpr auto wrp_refused(std::uint32_t programmed, std::uint32_t requested)
      -> bool
  {
    return requested != programmed and (programmed & wrp_unlock) == 0;
  }

  /**
   * @brief Whether the loaded protection covers a page: PSTRT <= page <= PEND of area A or B.
   * @param words Loaded words, wrp_registers order.
   * @param bank Bank of the page, 0 or 1, following SWAP_BANK.
   * @param page Page in its bank.
   * @param part Part name.
   */
  [[nodiscard]] static auto wrp_covers(
      std::span<std::uint32_t const> words,
      std::uint32_t                  bank,
      std::uint32_t                  page,
      std::string_view               part) -> bool
  {
    auto const top = watermark_top(part);
    return std::ranges::any_of(words.subspan(bank * 2, 2), [&](std::uint32_t word) {
      return (word & top) <= page and page <= (word >> 16 & top);
    });
  }

  /**
   * @brief Least LATENCY RM0456 Tables 54 and 55 ask for, see required_wait_states.
   * @param hclk HCLK in Hz.
   * @param range Voltage range, 1 to 4.
   * @param low_power FLASH_ACR.LPM.
   * @return The least LATENCY, empty when HCLK is above the maximum of the range.
   */
  [[nodiscard]] static auto wait_states(std::uint64_t hclk, std::uint32_t range, bool low_power)
      -> std::optional<std::uint32_t>
  {
    return required_wait_states(hclk, range, low_power);
  }

  /**
   * @brief The STM32U5 has no programming delay field.
   * @return Empty.
   */
  [[nodiscard]] static auto programming_delay(std::uint64_t, std::uint32_t)
      -> std::optional<std::uint32_t>
  {
    return std::nullopt;
  }

  /**
   * @brief FLASH_ECCR.ADDR_ECC of a failing quad-word, its byte offset in its bank.
   * @param line Physical address of the quad-word.
   * @param bank_base Physical address of its bank.
   */
  [[nodiscard]] static constexpr auto ecc_address(std::uint32_t line, std::uint32_t bank_base)
      -> std::uint32_t
  {
    return line - bank_base;
  }

  /**
   * @brief Whether a control word selects an erase: PER, MER1 or MER2, RM0456 7.3.6.
   * @param field Value of a set role in the control word.
   */
  template <typename Field> [[nodiscard]] static auto erase_selected(Field const& field) -> bool
  {
    return field(set_role::page_erase) != 0
        or field(set_role::bank1_erase) != 0
        or field(set_role::bank2_erase) != 0;
  }

  /**
   * @brief Whether a control word selects a page and a bank erase at once, RM0456 7.3.9.
   * @param field Value of a set role in the control word.
   */
  template <typename Field> [[nodiscard]] static auto erase_conflict(Field const& field) -> bool
  {
    return field(set_role::page_erase) != 0
       and (field(set_role::bank1_erase) != 0 or field(set_role::bank2_erase) != 0);
  }

  /**
   * @brief Erase a control word asks for, RM0456 7.3.6: PER with PNB and BKER, else MER1 or
   *        MER2 alone a bank, else both banks.
   * @param field Value of a set role in the control word.
   */
  template <typename Field>
  [[nodiscard]] static auto decode_erase(Field const& field) -> erase_command
  {
    if(field(set_role::page_erase) != 0)
    {
      return {
          .op   = operation::page_erase,
          .bank = field(set_role::bank_select),
          .page = field(set_role::page)};
    }
    if(field(set_role::bank1_erase) == 0 or field(set_role::bank2_erase) == 0)
    {
      return {.op = operation::bank_erase, .bank = field(set_role::bank1_erase) != 0 ? 0u : 1u};
    }
    return {.op = operation::mass_erase};
  }

  /**
   * @brief Encodes option bytes into the U5 option registers, after the checks of the class
   *        documentation.
   * @param part Part name, which selects the width of the page fields.
   * @param flash_size Size of the flash, both banks.
   * @param wanted Option bytes to encode.
   * @return The register values and the boot address, or the first refusal in field order.
   */
  [[nodiscard]] static auto
      encode_options(std::string_view part, std::uint32_t flash_size, option_bytes const& wanted)
          -> std::expected<option_words<option_registers.size()>, option_refusal>;
};

static_assert(names_every_role(u5_set_fields));
static_assert(names_every_role(u5_unit_fields));
static_assert(names_every_role(u5_privilege_fields));
static_assert(flag_matches_roles(u5_set_fields, u5_flash::burst_write, {set_role::burst}));
static_assert(flag_matches_roles(
    u5_set_fields,
    u5_flash::force_write,
    {set_role::force_write, set_role::strobe_error, set_role::inconsistency_error}));
static_assert(flag_matches_roles(
    u5_set_fields,
    not u5_flash::force_write,
    {set_role::size_error, set_role::alignment_error, set_role::program_error}));
static_assert(flag_matches_roles(
    u5_set_fields,
    u5_flash::erase_by_page,
    {set_role::page_erase, set_role::page, set_role::bank1_erase, set_role::bank2_erase}));
static_assert(flag_matches_roles(
    u5_set_fields,
    not u5_flash::erase_by_page,
    {set_role::sector_erase, set_role::sector, set_role::bank_erase, set_role::mass_erase}));
static_assert(flag_matches_roles(
    u5_set_fields,
    u5_flash::summary_error,
    {set_role::operation_error, set_role::error_irq}));
static_assert(flag_matches_roles(u5_unit_fields, u5_flash::option_load, {unit_role::option_load}));
static_assert(flag_matches_roles(
    u5_set_fields,
    u5_flash::per_error_irq,
    {set_role::protection_irq, set_role::sequence_irq, set_role::strobe_irq,
     set_role::inconsistency_irq}));

constexpr std::array<std::array<std::uint64_t, 6>, 4> vos_read_max_mhz{
    {
     {42, 84, 126, 168, 210, 250},
     {34, 68, 102, 136, 170, 200},
     {30, 60, 90, 120, 150, 0},
     {20, 40, 60, 80, 100, 0},
     }
};

constexpr std::array<error_enable, 4> h5_error_enables{
    {
     {.flag = set_role::protection_error, .enable = set_role::protection_irq},
     {.flag = set_role::sequence_error, .enable = set_role::sequence_irq},
     {.flag = set_role::strobe_error, .enable = set_role::strobe_irq},
     {.flag = set_role::inconsistency_error, .enable = set_role::inconsistency_irq},
     }
};

constexpr std::string_view u5_ip_version = "v1.0.0_U5_Cube";
constexpr std::string_view h5_ip_version = "v1.0.0_H5_Cube";

constexpr std::uint32_t optsr_production  = 0x2d30'edf8;
constexpr std::uint32_t optsr_swap_bank   = 1u << 31;
constexpr std::uint32_t optsr2_production = 0x0000'017c;
constexpr std::uint32_t tzen_enabled      = 0xb4;
constexpr std::uint32_t tzen_disabled     = 0xc3;
constexpr std::uint32_t boot_unlocked     = 0xc3;
constexpr std::uint32_t h5_boot_granule   = 256;
constexpr std::uint32_t sector_top        = 0x7f;

/** @brief STM32H5 FLASH traits, RM0481 section 7, each capability tied to its roles. */
struct h5_flash
{
  /** @brief No burst programming, RM0481 7.3.5. */
  static constexpr bool burst_write = false;
  /** @brief FW forces a partly filled write buffer out, RM0481 7.11.11. */
  static constexpr bool force_write = true;
  /** @brief SER, SNB, BER and MER select the erase, RM0481 7.3.6. */
  static constexpr bool erase_by_page = false;
  /** @brief No summary error flag, RM0481 7.9. */
  static constexpr bool summary_error = false;
  /** @brief No option byte loader reset, RM0481 7.4.2. */
  static constexpr bool option_load = false;
  /** @brief Each error flag has its own interrupt enable, RM0481 7.11.11. */
  static constexpr bool per_error_irq = true;
  /** @brief EOP sets at the end of every operation, RM0481 7.11.9. */
  static constexpr bool end_flag_always = true;
  /** @brief No oscillator is held for flash operations, RM0481 7.3.12. */
  static constexpr bool holds_hsi16 = false;
  /** @brief The option key sequence works with FLASH_NSCR locked, RM0481 7.6.7. */
  static constexpr bool option_key_needs_unlock = false;
  /** @brief A completed OPTSTRT reloads the _CUR registers, RM0481 7.4.3. */
  static constexpr bool option_reload_on_start = true;
  /** @brief Clearing ECCD clears FLASH_ECCDETR, RM0481 7.9.10. */
  static constexpr bool ecc_record_clears = true;
  /** @brief Control field the out-of-range sector warning names. */
  static constexpr std::string_view page_field = "SNB";
  /** @brief Datasheet times of an empty flash_options field. */
  static constexpr flash_timing timing = h5_flash_timing;
  /** @brief Error flags with their own interrupt enable, RM0481 7.11.11 bits 17 to 20. */
  static constexpr std::span<error_enable const> error_enables = h5_error_enables;
  /** @brief Bytes of one program, a 128-bit flash word, RM0481 7.3.5. */
  static constexpr std::uint32_t program_bytes = 16;
  /** @brief Bytes of the write buffer, one flash word. */
  static constexpr std::uint32_t buffer_bytes = program_bytes;
  /** @brief Bytes of an erase sector, RM0481 7.3.3. */
  static constexpr std::uint32_t page_bytes = 8 * 1024;
  /** @brief Section the latency warnings cite. */
  static constexpr std::string_view latency_section = "RM0481 Table 48";
  /** @brief Register sets, the Non-secure one first, then the Secure one. */
  static constexpr std::array<set_names, 2> set_registers{
      {
       {"FLASH_NSKEYR", "FLASH_NSCR", "FLASH_NSSR", "FLASH_NSCCR"},
       {"FLASH_SECKEYR", "FLASH_SECCR", "FLASH_SECSR", "FLASH_SECCCR"},
       }
  };
  /** @brief Role names of each register set. */
  static constexpr std::span<named_field<set_role> const> set_fields = h5_set_fields;
  /** @brief Role names of the fields outside the register sets. */
  static constexpr std::span<named_field<unit_role> const> unit_fields = h5_unit_fields;
  /** @brief Role names of the TrustZone privilege fields. */
  static constexpr std::span<named_field<privilege_role> const> privilege_fields =
      u5_privilege_fields;
  /** @brief Option registers loaded from flash_options::option_bytes, in option_words order. */
  static constexpr std::array<std::string_view, 8> option_registers{
      "FLASH_OPTSR_CUR",   "FLASH_OPTSR2_CUR",  "FLASH_NSBOOTR_CUR", "FLASH_SECBOOTR_CUR",
      "FLASH_SECWM1R_CUR", "FLASH_SECWM2R_CUR", "FLASH_HDP1R_CUR",   "FLASH_HDP2R_CUR",
  };
  /** @brief Secure option registers, RAZ/WI with TZEN = 0xC3, RM0481 7.6.1 and 7.11.27. */
  static constexpr std::array<std::string_view, 4> secure_option_registers{
      "FLASH_SECBOOTR_CUR",
      "FLASH_BOOTR_PRG",
      "FLASH_SECWM1R_PRG",
      "FLASH_SECWM2R_PRG",
  };
  /** @brief _PRG registers firmware writes for OPTSTRT, RM0481 7.4.3. */
  static constexpr std::array<std::string_view, 8> option_staging_registers{
      "FLASH_OPTSR_PRG",   "FLASH_OPTSR2_PRG",  "FLASH_NSBOOTR_PRG", "FLASH_BOOTR_PRG",
      "FLASH_SECWM1R_PRG", "FLASH_SECWM2R_PRG", "FLASH_HDP1R_PRG",   "FLASH_HDP2R_PRG",
  };
  /** @brief Write protection registers of bank 1 and bank 2, RM0481 7.11.35 to 7.11.49. */
  static constexpr std::array<wrp_names, 2> wrp_registers{
      {
       {"FLASH_WRP1R_PRG", "FLASH_WRP1R_CUR"},
       {"FLASH_WRP2R_PRG", "FLASH_WRP2R_CUR"},
       }
  };
  /** @brief Registers the dropped-option warning names as the ones OPTSTRT programs. */
  static constexpr std::string_view wrp_text = "FLASH_WRPxR_PRG";
  /** @brief Block-based security registers of bank 1, then bank 2, RM0481 7.11.29. */
  static constexpr std::array<std::string_view, 8> secure_block_registers{
      "FLASH_SECBB1R1", "FLASH_SECBB1R2", "FLASH_SECBB1R3", "FLASH_SECBB1R4",
      "FLASH_SECBB2R1", "FLASH_SECBB2R2", "FLASH_SECBB2R3", "FLASH_SECBB2R4",
  };
  /** @brief Block-based privilege registers of bank 1, then bank 2, RM0481 7.11.30. */
  static constexpr std::array<std::string_view, 8> privileged_block_registers{
      "FLASH_PRIVBB1R1", "FLASH_PRIVBB1R2", "FLASH_PRIVBB1R3", "FLASH_PRIVBB1R4",
      "FLASH_PRIVBB2R1", "FLASH_PRIVBB2R2", "FLASH_PRIVBB2R3", "FLASH_PRIVBB2R4",
  };
  /** @brief Block registers of one bank, 32 sectors each, 128 sectors per bank. */
  static constexpr std::size_t block_registers_per_bank = 4;
  /** @brief Index in option_registers of FLASH_SECWM1R_CUR and FLASH_SECWM2R_CUR. */
  static constexpr std::array<std::size_t, 2> watermark_words{4, 5};
  /** @brief Index in option_registers of FLASH_HDP1R_CUR and FLASH_HDP2R_CUR. */
  static constexpr std::array<std::size_t, 2> hide_words{6, 7};

  /**
   * @brief Whether a sector lies in the closed HDP area of its bank, RM0481 7.6.2 and Table 63:
   *        none at HDPL 1 or below, HDPx_STRT to HDPx_END at HDPL 2, and at HDPL 3 the lower of
   *        HDPx_STRT and HDPx_END to HDPx_END plus HDPx_EXT, when HDPx_END is not below
   *        HDPx_STRT or HDPx_EXT is above 0.
   * @param in Option words, extension, level and sector of the bank.
   */
  [[nodiscard]] static constexpr auto hidden(hide_inputs const& in) -> bool
  {
    auto const start = in.area & in.top;
    auto const end   = in.area >> 16 & in.top;
    if(in.level == 2)
    {
      return start <= in.page and in.page <= end;
    }
    if(in.level < 2 or (end < start and in.extension == 0))
    {
      return false;
    }
    return std::min(start, end) <= in.page and in.page <= end + in.extension;
  }

  /**
   * @brief Highest sector number of a SECWM or HDP field, RM0481 7.11.33.
   */
  [[nodiscard]] static constexpr auto watermark_top(std::string_view) -> std::uint32_t
  {
    return sector_top;
  }

  /** @brief Bits of an option register a firmware write is compared on: all of them. */
  [[nodiscard]] static constexpr auto option_mask(std::string_view, std::string_view)
      -> std::uint32_t
  {
    return ~0u;
  }

  /** @brief Delivery value of a WRPSG register, no group protected, RM0481 Table 55. */
  [[nodiscard]] static constexpr auto wrp_production(std::string_view) -> std::uint32_t
  {
    return ~0u;
  }

  /** @brief A written WRPSG word, all 32 bits stored. */
  [[nodiscard]] static constexpr auto wrp_normal(std::uint32_t word, std::string_view)
      -> std::uint32_t
  {
    return word;
  }

  /** @brief WRPSG bits change in either direction, RM0481 7.4.7 lists no rule for them. */
  [[nodiscard]] static constexpr auto wrp_refused(std::uint32_t, std::uint32_t) -> bool
  {
    return false;
  }

  /**
   * @brief Whether the loaded protection covers a sector: its WRPSG group bit is 0, bit N for
   *        sectors 4N to 4N+3, RM0481 7.11.35.
   * @param words Loaded words, bank 1 then bank 2.
   * @param bank Bank of the sector, 0 or 1.
   * @param page Sector in its bank.
   */
  [[nodiscard]] static auto wrp_covers(
      std::span<std::uint32_t const> words,
      std::uint32_t                  bank,
      std::uint32_t                  page,
      std::string_view) -> bool
  {
    return (words[bank] >> (page / 4) & 1u) == 0;
  }

  /**
   * @brief Least LATENCY RM0481 Table 48 asks for, the VOS level of D6 as the column.
   * @param hclk HCLK in Hz.
   * @param range VOS level plus 1, 1 for VOS0 to 4 for VOS3.
   * @return The least LATENCY, empty when HCLK is above the maximum of the level.
   */
  [[nodiscard]] static auto wait_states(std::uint64_t hclk, std::uint32_t range, bool)
      -> std::optional<std::uint32_t>
  {
    return first_wait_states(vos_read_max_mhz[range - 1], hclk);
  }

  /**
   * @brief Least FLASH_ACR.WRHIGHFREQ RM0481 Table 48 asks for: 0 up to 1 wait state, 1 for 2 and
   *        3, 2 for 4 and 5.
   * @param hclk HCLK in Hz.
   * @param range VOS level plus 1, 1 for VOS0 to 4 for VOS3.
   * @return The least WRHIGHFREQ, empty when HCLK is above the maximum of the level.
   */
  [[nodiscard]] static auto programming_delay(std::uint64_t hclk, std::uint32_t range)
      -> std::optional<std::uint32_t>
  {
    return first_wait_states(vos_read_max_mhz[range - 1], hclk).transform([](std::uint32_t ws) {
      return ws / 2;
    });
  }

  /**
   * @brief FLASH_ECCDETR.ADDR_ECC of a failing flash word, its 128-bit word index in its bank,
   *        0x0000 to 0xFFFF for a 1-Mbyte bank, RM0481 Table 88.
   * @param line Physical address of the flash word.
   * @param bank_base Physical address of its bank.
   */
  [[nodiscard]] static constexpr auto ecc_address(std::uint32_t line, std::uint32_t bank_base)
      -> std::uint32_t
  {
    return (line - bank_base) / program_bytes;
  }

  /**
   * @brief Whether a control word selects an erase: SER, BER or MER, RM0481 7.3.6.
   * @param field Value of a set role in the control word.
   */
  template <typename Field> [[nodiscard]] static auto erase_selected(Field const& field) -> bool
  {
    return field(set_role::sector_erase) != 0
        or field(set_role::bank_erase) != 0
        or field(set_role::mass_erase) != 0;
  }

  /**
   * @brief Whether a control word selects more than one of SER, BER and MER, RM0481 7.9.4.
   * @param field Value of a set role in the control word.
   */
  template <typename Field> [[nodiscard]] static auto erase_conflict(Field const& field) -> bool
  {
    return (field(set_role::sector_erase) != 0 ? 1 : 0)
             + (field(set_role::bank_erase) != 0 ? 1 : 0)
             + (field(set_role::mass_erase) != 0 ? 1 : 0)
         > 1;
  }

  /**
   * @brief Erase a control word asks for, RM0481 7.3.6: SER the sector SNB of the physical
   *        bank BKSEL, BER the bank BKSEL, MER both banks.
   * @param field Value of a set role in the control word.
   */
  template <typename Field>
  [[nodiscard]] static auto decode_erase(Field const& field) -> erase_command
  {
    if(field(set_role::sector_erase) != 0)
    {
      return {
          .op   = operation::page_erase,
          .bank = field(set_role::bank_select),
          .page = field(set_role::sector)};
    }
    if(field(set_role::bank_erase) != 0)
    {
      return {.op = operation::bank_erase, .bank = field(set_role::bank_select)};
    }
    return {.op = operation::mass_erase};
  }

  /**
   * @brief Encodes option bytes into the H5 _CUR option registers, RM0481 7.4 and 7.11.
   * @param flash_size Size of the flash, both banks.
   * @param wanted Option bytes to encode.
   * @return The register values and the boot address, or the first refusal in field order.
   */
  [[nodiscard]] static auto
      encode_options(std::string_view, std::uint32_t flash_size, option_bytes const& wanted)
          -> std::expected<option_words<option_registers.size()>, option_refusal>;
};

static_assert(names_every_role(h5_set_fields));
static_assert(names_every_role(h5_unit_fields));
static_assert(flag_matches_roles(h5_set_fields, h5_flash::burst_write, {set_role::burst}));
static_assert(flag_matches_roles(
    h5_set_fields,
    h5_flash::force_write,
    {set_role::force_write, set_role::strobe_error, set_role::inconsistency_error}));
static_assert(flag_matches_roles(
    h5_set_fields,
    not h5_flash::force_write,
    {set_role::size_error, set_role::alignment_error, set_role::program_error}));
static_assert(flag_matches_roles(
    h5_set_fields,
    h5_flash::erase_by_page,
    {set_role::page_erase, set_role::page, set_role::bank1_erase, set_role::bank2_erase}));
static_assert(flag_matches_roles(
    h5_set_fields,
    not h5_flash::erase_by_page,
    {set_role::sector_erase, set_role::sector, set_role::bank_erase, set_role::mass_erase}));
static_assert(flag_matches_roles(
    h5_set_fields,
    h5_flash::summary_error,
    {set_role::operation_error, set_role::error_irq}));
static_assert(flag_matches_roles(h5_unit_fields, h5_flash::option_load, {unit_role::option_load}));
static_assert(flag_matches_roles(
    h5_set_fields,
    h5_flash::per_error_irq,
    {set_role::protection_irq, set_role::sequence_irq, set_role::strobe_irq,
     set_role::inconsistency_irq}));

/**
 * @brief FLASH interface of the STM32U5, RM0456 section 7, and of the STM32H5, RM0481 section 7:
 *        the Non-secure register set, and under TrustZone the Secure register set, page
 *        security, HDP and page privilege. The traits give the role names, the register
 *        sets, the program, buffer and page sizes, the erase decode, the latency table, the
 *        ECC address, the option registers and their encoding, and the capability flags.
 *        u5_flash holds the STM32U5 values below, h5_flash the STM32H5 ones of the last
 *        section.
 *
 * Unlock, RM0456 7.3.5: KEY1 then KEY2 in FLASH_NSKEYR clears FLASH_NSCR.LOCK, any other write
 * is a bus error and keeps FLASH_NSCR locked until the next reset. A key write while unlocked
 * counts as a wrong sequence. FLASH_NSCR ignores writes while locked.
 *
 * Option unlock, RM0456 7.4.2: OPTLOCK is 1 at reset. With LOCK clear, OPTKEY1 then OPTKEY2 in
 * FLASH_OPTKEYR clear it. Any other write to FLASH_OPTKEYR, a key while LOCK is set or OPTLOCK is
 * clear included, returns a bus error and locks the sequence until the next reset, and OPTLOCK,
 * once set, stays set until then. RM0456 states the bus error for FLASH_NSKEYR only, RM0481
 * states it for FLASH_OPTKEYR too, and the model follows it. OPTLOCK is set-only, and setting
 * LOCK sets it. While OPTLOCK is set, OPTSTRT and OBL_LAUNCH ignore writes and the option
 * registers keep their value.
 *
 * Write protection, RM0456 7.6.1 and 7.9.19 to 7.9.24: FLASH_WRP1AR, FLASH_WRP1BR, FLASH_WRP2AR
 * and FLASH_WRP2BR hold UNLOCK in bit 31, PEND in bits 23:16 and PSTRT in bits 7:0, the fields as
 * wide as the SECWM fields of the part. Other bits read 1. The production value has PSTRT at the
 * field maximum and PEND at 0, which protects nothing: 0xFF80FFFF on the STM32U575. Writes
 * are stored while OPTLOCK is clear and ignored while it is set. OPTSTRT, with OPTLOCK clear and
 * no PGSERR or OPTWERR set, programs the four registers as the option bytes. A register whose
 * programmed UNLOCK is 0 cannot change, and one such change discards the whole modification,
 * reloads the registers from the programmed words and sets OPTWERR. The programmed words survive
 * every reset and the snapshot holds them. OPTSTRT completes at once, with no BSY. A page of bank
 * x, the bank number following SWAP_BANK, lies in area A or B of that bank when PSTRT <= page <=
 * PEND. A store, page erase, bank erase or mass erase that reaches such a page sets WRPERR in its
 * register set, drops the store and erases nothing. A power-on, brown-out or option byte loader
 * reset loads the programmed words into the protection and the WRP registers, RM0456 7.4.2. Any
 * other reset keeps the loaded words and resets the registers to them. OBL_LAUNCH, with OPTLOCK
 * clear, clears itself and requests an option byte loader reset, reset_kind::option_load, which
 * sets RCC_CSR.OBLRSTF and loads the registers from the programmed words. OPTSTRT programs the WRP
 * registers only: FLASH_OPTR, the boot addresses and the SECWM registers keep the machine's
 * option bytes, and a write to one of them that differs from the loaded word logs a
 * model_warning once at the next OPTSTRT.
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
 * at the end only with EOPIE. Geometry: without option bytes the model uses the production
 * FLASH_OPTR value 0x1FEFF8AA, RM0456 7.9.13: DUALBANK = 1, SWAP_BANK = 0, bank 2 at the flash
 * base plus half its size, 8 Kbyte pages. With flash_options::option_bytes, SWAP_BANK = 1 puts
 * bank 2 at the flash base and bank 1 at half its size, for BKER, MER1, MER2 and BK_ECC. A PNB at
 * or above the pages of a bank, which RM0456 leaves undefined, erases nothing, sets no flag and
 * logs a model_warning once, and raises model_error under strict_peripherals.
 *
 * Option bytes, RM0456 7.9.13 to 7.9.22: without flash_options::option_bytes, FLASH_OPTR,
 * FLASH_NSBOOTADDxR, FLASH_SECBOOTADD0R and FLASH_SECWMxRy are the description's registers, which
 * firmware writes as storage. A TrustZone machine without option bytes is refused at
 * construction, key `option_bytes`, since its page security would have no watermark. With them,
 * every reset loads the encoded option bytes into those registers, and writes are ignored.
 * Without them the registers also ignore writes while OPTLOCK is set. FLASH_OPTR reads the
 * production value with TZEN, DUALBANK and SWAP_BANK replaced. FLASH_NSBOOTADD1R reads its
 * production value 0x0BF9007F. Reserved bits read their production values, and so do the bits of a
 * SECWMx_PSTRT, SECWMx_PEND or HDPx_PEND field above the part's 7 bits, 5 on the STM32U535 and
 * U545, 8 on the U59x, U5Ax, U5Fx and U5Gx: FLASH_SECWM1R1 reads 0xFFFFFF80 for the whole bank on
 * the U575. Deviation on the U535 and U545: FLASH_SECWMxR1 reads 0xFFFFFFE0 for the whole bank,
 * bits 7:5 set above the 5-bit fields, where RM0456 7.9.17 and 7.9.21 give 0xFFFFFF80. The CMSIS
 * header masks the fields with 0x1F, and every other U535 row of those registers sets bits 7:5, so
 * the RM0456 value reads as the U575 row repeated. A whole-bank area encodes PSTRT = 0 and PEND =
 * the field maximum, RM0456 Table 58, no area PSTRT = the field maximum and PEND = 0, Table 61.
 * With TZEN = 0, FLASH_SECBOOTADD0R and FLASH_SECWMxRy are secure registers and read as zero,
 * RM0456 7.5.1. The checks, all made before the machine is built: watermark and HDP pages inside
 * the bank, an HDP area starting at the first page of its secure area and ending inside it, RM0456
 * Table 60, boot addresses aligned to 128 bytes. RM0456 Table 27 allows any boot address at RDP
 * level 0, so no range is checked. DUALBANK = 0, the single-bank organization, is not modelled and
 * refused.
 *
 * Errors, RM0456 7.3.9: PGSERR, SIZERR, PGAERR and PROGERR as its list states. For PROGERR the
 * 7.3.9 list wins over the FLASH_NSSR bit text, which accepts all-zero data over an all-zero
 * target: the model refuses it. A burst is checked per quad-word and programs nothing when one
 * of its 8 quad-words fails. Every error aborts the operation and flushes the write buffer.
 * With ERRIE an error also sets OPERR. FLASH_IRQn follows EOP with EOPIE and OPERR with ERRIE.
 * HSI16 is kept on while PG or STRT is set in either register set, through the RCC model.
 *
 * TrustZone, RM0456 7.5 and 7.7. With TZEN = 0 FLASH_SECKEYR, FLASH_SECSR, FLASH_SECCR,
 * FLASH_SECyBBRx and FLASH_SECHDPCR read as zero and ignore writes, RM0456 7.5.1, and
 * FLASH_PRIVCFGR and FLASH_PRIVyBBRx stay the description's storage. With TZEN = 1:
 * - The Secure set FLASH_SECKEYR, FLASH_SECCR and FLASH_SECSR works as the Non-secure one, with
 *   its own key sequence and lock, RM0456 7.3.5. One operation engine and one write buffer serve
 *   both sets: BSY and WDW read the same in both status registers. A program belongs to the set
 *   of the store's transaction, its first word fixing it for the buffer. RM0456 does not say what
 *   a store of the other set does to a filling buffer: the model sets PGSERR in the store's set
 *   and flushes the buffer. An erase belongs to the set whose STRT
 *   started it. Errors, EOP and OPERR go to the set of the operation, RM0456 7.3.9, and
 *   FLASH_S_IRQn follows the Secure EOP with EOPIE and OPERR with ERRIE, Table 78. A Secure
 *   access that sets the Non-secure STRT is a Non-secure PGSERR. An error of one set never ends
 *   an operation of the other: it sets its flags and leaves the running operation, its buffer
 *   and BSY alone.
 * - A page is Secure inside the SECWMx area of its bank or with its SECyBBRx bit set, RM0456
 *   7.5.2 and 7.5.4, banks following SWAP_BANK, 7.5.8. It is in the HDP area while HDPxEN is set,
 *   HDPx_ACCDIS is set and it lies from SECWMx_PSTRT to HDPx_PEND, 7.5.3. It is privileged with its
 *   PRIVyBBRx bit set, 7.5.6. The GTZC memory filter reads the same policy: the HDP area reads
 *   zero to every load and refuses every fetch, a privileged page does so to unprivileged
 *   accesses. Deviation: RM0456 Table 73 gives an unprivileged fetch of a privileged page zeros,
 *   the model a BusFault, as for an MPCBB block.
 * - A store whose transaction security differs from its page, into the HDP area, or
 *   unprivileged into a privileged page is dropped with WRPERR in its set, Tables 68 and 73. A
 *   page, bank or mass erase covering such a page, judged by the set and the privilege of the
 *   STRT write, erases nothing and sets WRPERR, Tables 68, 70 and 74: a bank that mixes Secure
 *   and Non-secure pages is refused whole. A security mismatch also raises the TZIC `FLASHIE`
 *   event.
 * - A Non-secure access to a Secure register reads zero, drops the write and raises the TZIC
 *   `FLASH_REGIE` event, an unprivileged one with SPRIV, or to a Non-secure register with
 *   NSPRIV, reads zero and drops the write, RM0456 Table 72. Only the registers the model serves
 *   follow NSPRIV: the rest of the block stays storage. SECyBBRx reads in any state, takes a
 *   Secure write, a Non-secure one being dropped with `FLASH_REGIE`, and from an unprivileged
 *   Secure write only the bits whose page is unprivileged, Table 75. PRIVyBBRx reads in any
 *   state, takes a privileged write only, and from a Non-secure one only the bits of Non-secure
 *   pages, Table 76. FLASH_PRIVCFGR takes NSPRIV from a privileged write and SPRIV from a Secure
 *   privileged one, 7.9.32. FLASH_SECHDPCR bits are set-only, cleared by every reset.
 * - A write that changes SECyBBRx, PRIVyBBRx or FLASH_SECHDPCR drops the engine's page verdicts
 *   and ends the engine run after the instruction, a reset or a restore drops them too. SECCR.INV
 *   inverts the flash security state of Table 62, Secure when a page is Secure, which the RCC
 *   FLASHEN and FLASHSMEN bits follow, RM0456 7.5.5.
 * - Deviation from RM0456 3.5.5: it calls a bank secure when one page is secure, which would let
 *   a Secure bank erase run over a mixed bank. The model follows Table 70 and refuses it whole.
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
 *
 * ECC, RM0456 7.3.2 and 7.9.11: a CPU or DMA load that reaches a torn quad-word, through either
 * flash alias, is a double error. With ECCD and ECCC clear it sets ECCD, BK_ECC to the bank
 * and ADDR_ECC to the quad-word's offset in its bank, and drives the NMI input. Once ECCD or
 * ECCC is set, a new error updates nothing. The NMI input stays active while ECCD is set, so a
 * handler that never clears ECCD is entered again on its return. Writing 1 to ECCD or ECCC
 * clears it, ECCIE is stored, the other fields are read-only. Every reset kind clears
 * FLASH_ECCR and lowers the NMI input, the torn quad-words stay. Deviations: host and debugger
 * reads, instruction fetches and vector table reads never detect an error, where silicon
 * raises the NMI on a debugger read too. ECCC is never set, as no single error is modelled, and
 * ECCIE has no effect. The model keeps no read buffer, so every load of a torn quad-word reports
 * once the flags are clear, where the 7.3.2 note lets a repeated read of a buffered line report
 * nothing.
 *
 * STM32H5, h5_flash, RM0481 section 7. The rules above hold with these differences:
 * - Program, RM0481 7.3.5: one 128-bit write buffer takes 8-, 16- and 32-bit stores into one
 *   flash word. WBNE is set while it fills, a full buffer starts the program, BSY set for
 *   flash_options::quad_word_program. A second store to a byte sets STRBERR, a store into another
 *   flash word sets INCERR, a store with PG clear or with SER, BER or MER set sets PGSERR, and
 *   each flushes the buffer and writes nothing, RM0481 7.9. A 16-bit store while WBNE is set
 *   fills the buffer like a word store, as measured on an STM32H573. FW programs a partly
 *   filled buffer with its unwritten bytes at 0xFF, and does nothing for a buffer of 0xFF bytes. A
 * program over a word that is not erased writes the data as given.
 * - Erase, RM0481 7.3.6: STRT with SER erases sector SNB of the physical bank BKSEL, with BER
 *   that bank, with MER both banks. STRT with none or more than one of them is PGSERR, 7.9.4.
 * - EOP sets at the end of every operation. FLASH_NSSR is read-only: FLASH_NSCCR clears its
 *   flags through the description's cross rules, 7.11.13. FLASH_IRQn follows EOP with EOPIE,
 *   WRPERR, PGSERR, STRBERR and INCERR each with its own enable, and OPTCHANGEERR with
 *   OPTCHANGEERRIE, 7.11.11.
 * - Option unlock, 7.4.3 and 7.11.8: OPTLOCK and OPTSTRT are bits 0 and 1 of FLASH_OPTCR, and
 *   the OPTKEYR sequence works with FLASH_NSCR locked. FLASH_OPTCR and the _PRG registers ignore
 *   writes while OPTLOCK is set. OPTSTRT programs FLASH_WRP1R_PRG and FLASH_WRP2R_PRG and loads
 *   them into FLASH_WRP1R_CUR and FLASH_WRP2R_CUR at once, with no BSY. Bit N of a WRPSG word
 *   protects sectors 4N to 4N+3 of its bank when 0, 7.11.35. The other _PRG registers are
 *   storage and log a model_warning once at the next OPTSTRT. No option byte loader reset.
 * - Option bytes, 7.4 and 7.11.19 to 7.11.52: every reset loads the encoded option bytes into
 *   FLASH_OPTSR_CUR, FLASH_OPTSR2_CUR, FLASH_NSBOOTR_CUR, FLASH_SECBOOTR_CUR, FLASH_SECWMxR_CUR
 *   and FLASH_HDPxR_CUR, from the delivery values of Table 55: TZEN 0xB4 or 0xC3, boot
 *   addresses with their locks at 0xC3, a whole-bank watermark as STRT 0 END 0x7F, no area as
 *   STRT 0x7F END 0, no HDP area as STRT 1 END 0. Without option bytes the eight registers
 *   reset to those delivery values of the description. FLASH_OPTCR.SWAP_BANK
 *   reads SWAP_BANK. The checks: sectors inside the bank, boot addresses aligned to 256 bytes,
 *   DUALBANK set. With TZEN = 0xC3, with or without option bytes, FLASH_SECBOOTR_CUR,
 *   FLASH_BOOTR_PRG, FLASH_SECWM1R_PRG, FLASH_SECWM2R_PRG and the Secure set read zero and
 *   ignore writes, 7.6.1.
 * - TrustZone, 7.6.1 to 7.6.4: a sector is Secure inside FLASH_SECWMxR_CUR, SECWMx_STRT to
 *   SECWMx_END, or with its FLASH_SECBBxRy bit set, 32 sectors per register and 4 registers per
 *   bank, and privileged with its FLASH_PRIVBBxRy bit set. The HDP area of a bank is
 *   FLASH_HDPxR_CUR, HDPx_STRT to HDPx_END, independent of the watermark. The SBS HDPL level
 *   closes it, Table 63: at HDPL 2 from HDPx_STRT to HDPx_END, at HDPL 3 from the lower of the
 *   two to HDPx_END plus the HDPx_EXT sectors of FLASH_HDPEXTR. FLASH_HDPEXTR fields only grow
 *   and ignore writes at HDPL 3, 7.11.18. A closed sector reads zero and refuses every fetch, as
 *   the U5 HDP area does.
 * - ECC, 7.9.10: ECCD and its record are in FLASH_ECCDETR, ADDR_ECC the 128-bit word index in
 *   the bank, and clearing ECCD clears the record. ECCC is in FLASH_ECCCORR.
 * - Latency: RM0481 Table 48 per VOS level, with no LPM. No oscillator is held for programming.
 * - Timings default to DS14121 Rev 5 Table 52, h5_flash_timing.
 */
template <typename Traits>
class flash_model final : public dispatch_model<flash_model<Traits>, flash_state>,
                          public clock_listener,
                          public voltage_range_listener,
                          public secure_page_controller,
                          public trustzone_aware
{
  using base = dispatch_model<flash_model<Traits>, flash_state>;
  using base::apply_crosses;
  using base::crosses_from;
  static_assert(Traits::buffer_bytes / 4 <= buffer_words);
  static_assert(Traits::buffer_bytes % Traits::program_bytes == 0);
  static_assert(names_control_or_status(Traits::set_fields));
  using base::claims;
  using base::ctx_;
  using base::desc_;
  using base::describe;
  using base::field;
  using base::has_register;
  using base::irq;
  using base::reg;
  using base::schedule_in;
  using base::state_;
  using base::warn;

public:
  flash_model(model_context& ctx, peripheral_desc const& desc, flash_options const& options);

  void reset(register_file& regs, reset_kind kind) override;
  void power_cut() override;
  void save(std::vector<std::byte>& out) const override;
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
  void on_memory_read(region_desc const& region, address addr, std::uint8_t size, access_ctx ctx)
      override;

  void on_clock_change() override;
  void on_voltage_range_change() override;

  [[nodiscard]] auto page_secure(address at) const -> std::optional<bool> override;
  [[nodiscard]] auto page_rule(address at) const -> std::optional<page_policy> override;
  [[nodiscard]] auto secure_page_size() const -> std::uint32_t override;
  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;

  void               access_written(std::uint32_t old, std::uint32_t now);
  void               key_written(std::uint32_t old, std::uint32_t now);
  void               secure_key_written(std::uint32_t old, std::uint32_t now);
  void               option_key_written(std::uint32_t old, std::uint32_t now);
  void               control_written(std::uint32_t old, std::uint32_t now);
  void               secure_control_written(std::uint32_t old, std::uint32_t now);
  void               status_written(std::uint32_t old, std::uint32_t now);
  void               secure_status_written(std::uint32_t old, std::uint32_t now);
  void               clear_written(std::uint32_t old, std::uint32_t now);
  void               secure_clear_written(std::uint32_t old, std::uint32_t now);
  void               option_control_written(std::uint32_t old, std::uint32_t now);
  void               staging_written(std::uint32_t old, std::uint32_t now);
  void               ecc_written(std::uint32_t old, std::uint32_t now);
  void               option_written(std::uint32_t old, std::uint32_t now);
  void               wrp_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto option_locked() const -> bool;
  void               secure_block_written(std::uint32_t old, std::uint32_t now);
  void               hide_written(std::uint32_t old, std::uint32_t now);
  void               extension_written(std::uint32_t old, std::uint32_t now);
  void               privilege_written(std::uint32_t old, std::uint32_t now);
  void               privileged_block_written(std::uint32_t old, std::uint32_t now);

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

private:
  struct register_set
  {
    reg_ref                key;
    reg_ref                control;
    reg_ref                status;
    field_table<set_role>  fields;
    std::uint32_t          clearable{};
    std::vector<cross_ref> clears{};
  };

  [[nodiscard]] auto make_set(std::size_t index) const -> register_set;
  template <typename Role>
  [[nodiscard]] auto make_table(std::span<named_field<Role> const> names) const
      -> field_table<Role>;
  [[nodiscard]] auto has_field(std::string_view reg, std::string_view name) const -> bool;
  [[nodiscard]] auto option_field(unit_role role, std::uint32_t control) const
      -> std::optional<field_ref>;
  [[nodiscard]] auto field_of(bool secure, set_role role) const -> field_ref;
  [[nodiscard]] auto value_of(bool secure, set_role role, std::uint32_t word) const
      -> std::uint32_t;
  [[nodiscard]] auto unit_value(unit_role role) const -> std::uint32_t;
  [[nodiscard]] auto flag(bool secure, set_role role) const -> bool;
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto errors_pending(bool secure) const -> bool;
  void               fail(bool secure, set_role role);
  void               fail(bool secure, field_ref flagged);
  void               set_busy(bool on);
  void               set_waiting(bool on);
  void               interrupt();
  void               finish();
  void               store(held_store const& one, std::uint8_t attrs);
  void               fill(std::uint32_t addr, std::uint8_t size, std::uint32_t value, bool secure);
  void               force(bool secure);
  void               hold(held_store const& one, std::uint8_t attrs);
  void start(operation which, std::uint32_t target, std::uint32_t length, bool secure);
  void start_erase(bool secure, std::uint32_t control);
  [[nodiscard]] auto duration_of(operation which, std::uint32_t length) const
      -> std::chrono::nanoseconds;
  [[nodiscard]] auto refused(
      std::uint32_t              at,
      std::span<std::byte const> target,
      std::span<std::byte const> data) const -> bool;
  [[nodiscard]] auto buffered() const -> std::array<std::byte, Traits::buffer_bytes>;
  void               tear_program(std::uint32_t full);
  void               tear_erase(std::uint32_t full);
  void               forget_torn(std::uint32_t from, std::uint32_t length);
  void               sync_watches();
  void               report_double_error(std::uint32_t line);
  [[nodiscard]] auto banks() const -> bank_geometry;
  [[nodiscard]] auto bank_of(std::uint32_t at) const -> std::uint32_t;
  [[nodiscard]] auto bank_base(std::uint32_t at) const -> std::uint32_t;
  [[nodiscard]] auto bank_start(std::uint32_t bank) const -> std::uint32_t;
  [[nodiscard]] auto rule_of(std::uint32_t at) const -> page_policy;
  [[nodiscard]] auto block_bit(std::span<std::uint32_t const> words, std::uint32_t at) const
      -> bool;
  [[nodiscard]] auto
      writable(std::uint32_t from, std::uint32_t length, bool secure, bool privileged) -> bool;
  [[nodiscard]] auto register_refused(std::uint32_t word, access_ctx ctx, bool write) -> bool;
  void               control_written(bool secure, std::uint32_t old, std::uint32_t now);
  void               key_written(bool secure, std::uint32_t now);
  void               status_written(bool secure, std::uint32_t old);
  void               policy_written(std::uint32_t old, std::uint32_t now);
  void               illegal_access(std::string_view enable_field);
  [[nodiscard]] auto hsi16_held() const -> bool;
  void               program(bool secure);
  void               hold_hsi16(bool held);
  [[nodiscard]] auto wrp_programmed(std::size_t index) const -> std::uint32_t;
  [[nodiscard]] auto option_loaded(std::uint32_t offset) const -> std::uint32_t;
  [[nodiscard]] auto option_mask(std::uint32_t offset) const -> std::uint32_t;
  void               warn_dropped_options();
  [[nodiscard]] auto wrp_protected(std::uint32_t from, std::uint32_t length) const -> bool;
  [[nodiscard]] static auto loads_options(reset_kind kind) -> bool;
  void                      poke_wrp();
  void                      load_options();
  void                      program_options();
  void                      check_latency(std::string_view trigger);
  void                      check_programming_delay(std::string_view trigger);
  [[nodiscard]] auto        active_range() const -> std::uint32_t;

  flash_options                                                     options_;
  register_file&                                                    regs_;
  region_desc const*                                                storage_{};
  field_table<unit_role>                                            unit_;
  std::array<register_set, Traits::set_registers.size()>            sets_;
  irq_line&                                                         line_;
  irq_line*                                                         secure_line_{};
  core::clock_domain                                                hclk_;
  std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> latency_logged_;
  std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> delay_logged_;
  std::vector<std::uint32_t>                                        watched_;
  std::uint32_t                                                     written_{};
  std::uint32_t                                                     written_word_{};
  access_ctx                                                        access_{};
  bool                                                              rejected_{};
  bool                                                              page_logged_{};
  bool                                                              option_logged_{};
  std::vector<std::uint32_t>                                        option_changed_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>>              option_words_;
  std::vector<std::uint32_t>                                        option_offsets_;
  std::vector<std::uint32_t>                                        wrp_offsets_;
  std::vector<std::uint32_t>                                        wrp_active_offsets_;
  bool                                                              swapped_{};
  std::vector<std::uint32_t>                                        secure_words_;
  std::vector<std::uint32_t>                                        secure_blocks_;
  std::vector<std::uint32_t>                                        privileged_blocks_;
  std::optional<field_table<privilege_role>>                        privilege_;
  std::optional<std::uint32_t>                                      privilege_word_;
  secure_flash_state                                                secure_;
  illegal_access_controller*                                        events_{};
  bool                                                              events_linked_{};
};

template <typename Traits>
auto flash_table(peripheral_desc const& desc, flash_options const& options, bool trustzone)
    -> std::vector<write_entry<flash_model<Traits>>>
{
  using model                     = flash_model<Traits>;
  constexpr auto access_register  = register_of(Traits::unit_fields, unit_role::latency);
  constexpr auto ecc_register     = register_of(Traits::unit_fields, unit_role::ecc_detect);
  constexpr auto hide_register    = register_of(Traits::unit_fields, unit_role::hide_bank1);
  constexpr auto correct_register = register_of(Traits::unit_fields, unit_role::ecc_correct);
  constexpr auto option_register  = register_of(Traits::unit_fields, unit_role::option_lock);
  auto const&    nonsecure        = Traits::set_registers[0];
  std::vector<write_entry<model>> table{
      {access_register,   &model::access_written    },
      {nonsecure.key,     &model::key_written       },
      {"FLASH_OPTKEYR",   &model::option_key_written},
      {nonsecure.control, &model::control_written   },
      {nonsecure.status,  &model::status_written    },
      {ecc_register,      &model::ecc_written       },
  };
  if constexpr(correct_register != ecc_register)
  {
    table.push_back({correct_register, &model::ecc_written});
  }
  if constexpr(option_register != Traits::set_registers[0].control)
  {
    table.push_back({option_register, &model::option_control_written});
  }
  if constexpr(not Traits::set_registers[0].clear.empty())
  {
    table.push_back({nonsecure.clear, &model::clear_written});
  }
  if constexpr(Traits::set_registers.size() > 1)
  {
    auto const& secure = Traits::set_registers[1];
    table.push_back({secure.key, &model::secure_key_written});
    table.push_back({secure.control, &model::secure_control_written});
    table.push_back({secure.status, &model::secure_status_written});
    if constexpr(not Traits::set_registers[1].clear.empty())
    {
      table.push_back({secure.clear, &model::secure_clear_written});
    }
    if constexpr(not hide_register.empty())
    {
      table.push_back({hide_register, &model::hide_written});
    }
  }
  for(auto const name : Traits::secure_block_registers)
  {
    if(find_register(desc, name) != nullptr)
    {
      table.push_back({name, &flash_model<Traits>::secure_block_written});
    }
  }
  if(trustzone)
  {
    table.push_back({"FLASH_PRIVCFGR", &flash_model<Traits>::privilege_written});
    if constexpr(
        constexpr auto extension = register_of(Traits::unit_fields, unit_role::hide_extension1);
        not extension.empty())
    {
      table.push_back({extension, &flash_model<Traits>::extension_written});
    }
    for(auto const name : Traits::privileged_block_registers)
    {
      if(find_register(desc, name) != nullptr)
      {
        table.push_back({name, &flash_model<Traits>::privileged_block_written});
      }
    }
  }
  for(auto const name : Traits::option_registers)
  {
    if(options.option_bytes or find_register(desc, name) != nullptr)
    {
      table.push_back({name, &flash_model<Traits>::option_written});
    }
  }
  for(auto const name : Traits::option_staging_registers)
  {
    if(find_register(desc, name) != nullptr)
    {
      table.push_back({name, &flash_model<Traits>::staging_written});
    }
  }
  if(std::ranges::all_of(Traits::wrp_registers, [&desc](wrp_names const& names) {
       return find_register(desc, names.program) != nullptr
          and find_register(desc, names.active) != nullptr;
     }))
  {
    for(auto const& names : Traits::wrp_registers)
    {
      table.push_back({names.program, &flash_model<Traits>::wrp_written});
      if(names.active != names.program)
      {
        table.push_back({names.active, nullptr});
      }
    }
  }
  return table;
}

template <typename Traits>
flash_model<Traits>::flash_model(
    model_context&         ctx,
    peripheral_desc const& desc,
    flash_options const&   options)
    : base{ctx, desc, "FLASH", flash_table<Traits>(desc, options, ctx.trustzone)},
      options_{options}, regs_{ctx.regs}, unit_{make_table<unit_role>(Traits::unit_fields)},
      sets_{[this]<std::size_t... index>(std::index_sequence<index...>) {
        return std::array{make_set(index)...};
      }(std::make_index_sequence<Traits::set_registers.size()>{})},
      line_{irq("FLASH_IRQn")}, hclk_{ctx.clock.domain("hclk")}
{
  if(ctx.trustzone)
  {
    secure_line_    = &irq("FLASH_S_IRQn");
    privilege_      = make_table<privilege_role>(Traits::privilege_fields);
    privilege_word_ = (*privilege_)[privilege_role::nonsecure].offset;
  }
  if constexpr(Traits::set_registers.size() > 1)
  {
    auto const& secure_set = sets_[1];
    secure_words_.push_back(secure_set.key.offset);
    secure_words_.push_back(secure_set.status.offset);
    secure_words_.push_back(secure_set.control.offset);
    if constexpr(not Traits::set_registers[1].clear.empty())
    {
      secure_words_.push_back(reg(Traits::set_registers[1].clear).offset);
    }
    if constexpr(not register_of(Traits::unit_fields, unit_role::hide_bank1).empty())
    {
      secure_words_.push_back(unit_[unit_role::hide_bank1].offset);
    }
  }
  for(auto const name : Traits::option_registers)
  {
    if(has_register(name))
    {
      option_offsets_.push_back(reg(name).offset);
    }
  }
  if(std::ranges::all_of(Traits::wrp_registers, [this](wrp_names const& names) {
       return has_register(names.program) and has_register(names.active);
     }))
  {
    for(auto const& names : Traits::wrp_registers)
    {
      wrp_offsets_.push_back(reg(names.program).offset);
      wrp_active_offsets_.push_back(reg(names.active).offset);
      option_offsets_.push_back(reg(names.program).offset);
      if(names.active != names.program)
      {
        option_offsets_.push_back(reg(names.active).offset);
      }
    }
    state_.wrp.assign(wrp_offsets_.size(), Traits::wrp_production(ctx.part.part));
    state_.wrp_active = state_.wrp;
  }
  for(auto const name : Traits::secure_block_registers)
  {
    secure_blocks_.push_back(has_register(name) ? reg(name).offset : ~0u);
  }
  for(auto const name : Traits::privileged_block_registers)
  {
    privileged_blocks_.push_back(has_register(name) ? reg(name).offset : ~0u);
  }
  auto const own = std::ranges::find_if(ctx.part.regions, [&desc](region_desc const& region) {
    return region.controller == desc.name and not region.physical;
  });
  if(own != ctx.part.regions.end())
  {
    storage_          = &*own;
    auto const needed = (banks().size / Traits::page_bytes + pages_per_block_register - 1)
                      / pages_per_block_register;
    for(std::size_t bank = 0; bank < 2; ++bank)
    {
      for(std::size_t index = 0; index < needed; ++index)
      {
        auto const at = bank * Traits::block_registers_per_bank + index;
        this->expect_register(Traits::secure_block_registers[at], true);
        this->expect_register(Traits::privileged_block_registers[at], true);
      }
    }
  }
  for(auto const name : Traits::secure_option_registers)
  {
    secure_words_.push_back(reg(name).offset);
  }
  if(not options_.option_bytes)
  {
    if(ctx.trustzone)
    {
      throw config_error(
          desc.name, 0, "option_bytes",
          "the FLASH option bytes of a TrustZone machine, set with configure_option_bytes");
    }
    return;
  }
  if(options_.option_bytes->tzen != ctx.trustzone)
  {
    throw config_error(
        desc.name, 0, "option_bytes.tzen",
        std::format("TZEN {}, as machine_options::trustzone", ctx.trustzone ? 1 : 0));
  }
  if(storage_ == nullptr)
  {
    throw config_error(
        desc.name, 0, "option_bytes", "a flash region that names FLASH as its controller");
  }
  auto const words = Traits::encode_options(ctx.part.part, storage_->size, *options_.option_bytes);
  if(not words)
  {
    throw config_error(desc.name, 0, words.error().key, words.error().expected);
  }
  for(auto const [name, value] : std::views::zip(Traits::option_registers, words->words))
  {
    option_words_.emplace_back(reg(name).offset, value);
  }
  swapped_ = options_.option_bytes->swap_bank;
}

template <typename Traits>
auto flash_model<Traits>::has_field(std::string_view reg, std::string_view name) const -> bool
{
  auto const* const found = find_register(desc_, reg);
  return found != nullptr and std::ranges::contains(found->fields, name, &field_desc::name);
}

template <typename Traits>
template <typename Role>
auto flash_model<Traits>::make_table(std::span<named_field<Role> const> names) const
    -> field_table<Role>
{
  return {
      names, [this](std::string_view reg, std::string_view name) { return field(reg, name); },
      [this](std::string_view reg, std::string_view name) { return has_field(reg, name); }};
}

template <typename Traits>
auto flash_model<Traits>::option_field(unit_role role, std::uint32_t control) const
    -> std::optional<field_ref>
{
  auto const found = unit_.find(role);
  return found and found->offset == control ? found : std::nullopt;
}

template <typename Traits>
auto flash_model<Traits>::make_set(std::size_t index) const -> register_set
{
  auto const& names = Traits::set_registers[index];
  auto const  named = [&names](std::string_view reg) {
    return reg == "CR" ? names.control : names.status;
  };
  field_table<set_role> fields{
      Traits::set_fields,
      [&](std::string_view reg, std::string_view name) { return field(named(reg), name); },
      [&](std::string_view reg, std::string_view name) { return has_field(named(reg), name); }};
  std::uint32_t clearable = 0;
  for(auto const role :
      {set_role::end, set_role::operation_error, set_role::program_error,
       set_role::protection_error, set_role::alignment_error, set_role::size_error,
       set_role::sequence_error, set_role::strobe_error, set_role::inconsistency_error})
  {
    if(auto const found = fields.find(role))
    {
      clearable |= found->mask();
    }
  }
  auto const status = reg(names.status);
  if(auto const option_error = option_field(unit_role::option_error, status.offset))
  {
    clearable |= option_error->mask();
  }
  if(not names.clear.empty())
  {
    clearable = 0;
  }
  return {
      .key       = reg(names.key),
      .control   = reg(names.control),
      .status    = status,
      .fields    = fields,
      .clearable = clearable,
      .clears    = names.clear.empty() ? std::vector<cross_ref>{} : crosses_from(names.clear)};
}

auto u5_flash::encode_options(
    std::string_view    part,
    std::uint32_t       flash_size,
    option_bytes const& wanted)
    -> std::expected<option_words<option_registers.size()>, option_refusal>
{
  auto const refuse = [](std::string_view field, std::string expected) {
    return std::unexpected{
        option_refusal{
                       .key = std::format("option_bytes.{}", field), .expected = std::move(expected)}
    };
  };
  auto const pages  = flash_size / 2 / page_bytes;
  auto const top    = (1u << watermark_bits(part)) - 1;
  auto const unused = 0xffu & ~top;
  struct area
  {
    std::uint32_t first{};
    std::uint32_t last{};
    std::uint32_t encoded_last{};
  };
  auto const area_of =
      [&](watermark const& wanted_area,
          std::string_view field) -> std::expected<std::optional<area>, option_refusal> {
    if(std::holds_alternative<no_area>(wanted_area))
    {
      return std::nullopt;
    }
    if(std::holds_alternative<whole_bank>(wanted_area))
    {
      return area{.first = 0, .last = pages - 1, .encoded_last = top};
    }
    auto const range = std::get<page_range>(wanted_area);
    if(range.first > range.last or range.last >= pages)
    {
      return refuse(
          field, std::format(
                     "a first and a last page from 0 to {}, the {} pages of a bank, not {} to {}",
                     pages - 1, pages, range.first, range.last));
    }
    return area{.first = range.first, .last = range.last, .encoded_last = range.last};
  };
  auto const first_word = [&](std::optional<area> const& secure) {
    auto const [start, end] =
        secure ? std::pair{secure->first, secure->encoded_last} : std::pair{top, std::uint32_t{0}};
    return secwm_r1_reserved | unused << 16 | unused | end << 16 | start;
  };
  auto const second_word =
      [&](std::optional<area> const& secure, std::optional<page_range> const& hidden,
          std::string_view field) -> std::expected<std::uint32_t, option_refusal> {
    auto const word = secwm_r2_reserved | unused << 16 | unused;
    if(not hidden)
    {
      return word;
    }
    if(not secure)
    {
      return refuse(field, "a hide-protection area inside a secure area, RM0456 Table 60");
    }
    if(hidden->first != secure->first
       or hidden->last < hidden->first
       or hidden->last > secure->last)
    {
      return refuse(
          field, std::format(
                     "a hide-protection area from page {}, the first of its secure area, to at "
                     "most page {}, RM0456 Table 60, not {} to {}",
                     secure->first, secure->last, hidden->first, hidden->last));
    }
    return word | hdp_enable | hidden->last << 16;
  };
  auto const boot_of =
      [&](std::optional<std::uint32_t> given, std::uint32_t fallback, std::string_view field,
          std::string_view section) -> std::expected<std::uint32_t, option_refusal> {
    auto const at = given.value_or(fallback);
    if(at % boot_granule != 0)
    {
      return refuse(
          field,
          std::format("an address aligned to 128 bytes, RM0456 {}, not {:#010x}", section, at));
    }
    return at;
  };
  auto const secure1 = area_of(wanted.secwm1, "secwm1");
  if(not secure1)
  {
    return std::unexpected{secure1.error()};
  }
  auto const secure2 = area_of(wanted.secwm2, "secwm2");
  if(not secure2)
  {
    return std::unexpected{secure2.error()};
  }
  auto const hidden1 = second_word(*secure1, wanted.hdp1, "hdp1");
  if(not hidden1)
  {
    return std::unexpected{hidden1.error()};
  }
  auto const hidden2 = second_word(*secure2, wanted.hdp2, "hdp2");
  if(not hidden2)
  {
    return std::unexpected{hidden2.error()};
  }
  auto const secboot = boot_of(wanted.secboot, secure_boot_default, "secboot", "7.9.16");
  if(not secboot)
  {
    return std::unexpected{secboot.error()};
  }
  auto const nsboot = boot_of(wanted.nsboot, nonsecure_boot_default, "nsboot", "7.9.14");
  if(not nsboot)
  {
    return std::unexpected{nsboot.error()};
  }
  if(not wanted.dualbank.value_or(true))
  {
    return refuse(
        "dualbank", "a dual-bank flash: the single-bank organization of DUALBANK = 0 is not "
                    "modelled");
  }
  auto const optr        = (optr_production & ~(optr_tzen | optr_swap_bank))
                         | (wanted.tzen ? optr_tzen : 0)
                         | optr_dualbank
                         | (wanted.swap_bank ? optr_swap_bank : 0);
  auto const secure_word = [&wanted](std::uint32_t word) { return wanted.tzen ? word : 0; };
  return option_words<option_registers.size()>{
      .words =
          {optr, *nsboot | nsboot_reserved, nsbootadd1_production,
                  secure_word(*secboot | secboot_reserved), secure_word(first_word(*secure1)),
                  secure_word(*hidden1), secure_word(first_word(*secure2)), secure_word(*hidden2)},
      .boot = wanted.tzen ? *secboot : *nsboot,
  };
}

auto h5_flash::encode_options(
    std::string_view,
    std::uint32_t       flash_size,
    option_bytes const& wanted)
    -> std::expected<option_words<option_registers.size()>, option_refusal>
{
  auto const refuse = [](std::string_view field, std::string expected) {
    return std::unexpected{
        option_refusal{
                       .key = std::format("option_bytes.{}", field), .expected = std::move(expected)}
    };
  };
  auto const sectors = flash_size / 2 / page_bytes;
  auto const range_word =
      [&](page_range const& range,
          std::string_view  field) -> std::expected<std::uint32_t, option_refusal> {
    if(range.first > range.last or range.last >= sectors)
    {
      return refuse(
          field, std::format(
                     "a first and a last sector from 0 to {}, the {} sectors of a bank, RM0481 "
                     "7.11.33, not {} to {}",
                     sectors - 1, sectors, range.first, range.last));
    }
    return range.last << 16 | range.first;
  };
  auto const watermark_word =
      [&](watermark const& area,
          std::string_view field) -> std::expected<std::uint32_t, option_refusal> {
    if(std::holds_alternative<no_area>(area))
    {
      return sector_top;
    }
    if(std::holds_alternative<whole_bank>(area))
    {
      return (sectors - 1) << 16;
    }
    return range_word(std::get<page_range>(area), field);
  };
  auto const hide_word =
      [&](std::optional<page_range> const& area,
          std::string_view                 field) -> std::expected<std::uint32_t, option_refusal> {
    return area ? range_word(*area, field) : std::expected<std::uint32_t, option_refusal>{1u};
  };
  auto const boot_of =
      [&](std::optional<std::uint32_t> given, std::uint32_t fallback, std::string_view field,
          std::string_view section) -> std::expected<std::uint32_t, option_refusal> {
    auto const at = given.value_or(fallback);
    if(at % h5_boot_granule != 0)
    {
      return refuse(
          field,
          std::format("an address aligned to 256 bytes, RM0481 {}, not {:#010x}", section, at));
    }
    return at;
  };
  auto const secure1 = watermark_word(wanted.secwm1, "secwm1");
  if(not secure1)
  {
    return std::unexpected{secure1.error()};
  }
  auto const secure2 = watermark_word(wanted.secwm2, "secwm2");
  if(not secure2)
  {
    return std::unexpected{secure2.error()};
  }
  auto const hidden1 = hide_word(wanted.hdp1, "hdp1");
  if(not hidden1)
  {
    return std::unexpected{hidden1.error()};
  }
  auto const hidden2 = hide_word(wanted.hdp2, "hdp2");
  if(not hidden2)
  {
    return std::unexpected{hidden2.error()};
  }
  auto const secboot = boot_of(wanted.secboot, secure_boot_default, "secboot", "7.11.27");
  if(not secboot)
  {
    return std::unexpected{secboot.error()};
  }
  auto const nsboot = boot_of(wanted.nsboot, nonsecure_boot_default, "nsboot", "7.11.25");
  if(not nsboot)
  {
    return std::unexpected{nsboot.error()};
  }
  if(not wanted.dualbank.value_or(true))
  {
    return refuse("dualbank", "a dual-bank flash, RM0481 7.2 has no single-bank organization");
  }
  return option_words<option_registers.size()>{
      .words =
          {optsr_production | (wanted.swap_bank ? optsr_swap_bank : 0),
                  optsr2_production | (wanted.tzen ? tzen_enabled : tzen_disabled) << 24,
                  *nsboot | boot_unlocked, *secboot | boot_unlocked, *secure1, *secure2, *hidden1,
                  *hidden2},
      .boot = wanted.tzen ? *secboot : *nsboot,
  };
}

template <typename Traits> void flash_model<Traits>::reset(register_file& regs, reset_kind kind)
{
  for(auto const& name : claims())
  {
    auto const& described = describe(name);
    regs.poke(reg(name).offset, described.reset & described.reset_mask);
  }
  auto const generation = state_.generation;
  auto       torn       = std::move(state_.torn);
  auto       wrp        = std::move(state_.wrp);
  auto       wrp_active = std::move(state_.wrp_active);
  state_                = {};
  state_.generation     = generation + 1;
  state_.torn           = std::move(torn);
  state_.wrp            = std::move(wrp);
  state_.wrp_active     = loads_options(kind) ? state_.wrp : std::move(wrp_active);
  option_changed_.clear();
  secure_ = {};
  for(auto const [offset, value] : option_words_)
  {
    regs.poke(offset, value);
  }
  poke_wrp();
  if(auto const swap = unit_.find(unit_role::option_swap))
  {
    update(regs, *swap, swapped_ ? 1 : 0);
  }
  line_.lower();
  if(secure_line_ != nullptr)
  {
    secure_line_->lower();
    ctx_.services.memory_policy_changed();
  }
  ctx_.nmi.lower();
}

template <typename Traits>
auto flash_model<Traits>::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     ctx) -> std::expected<std::uint32_t, bus_error>
{
  auto const word = offset & ~3u;
  if(register_refused(word, ctx, false))
  {
    return 0;
  }
  if(std::ranges::contains(option_offsets_, word))
  {
    return read_lanes(regs.peek(word), offset, size);
  }
  return base::on_read(regs, offset, size, ctx);
}

template <typename Traits>
void flash_model<Traits>::option_written(std::uint32_t old, std::uint32_t now)
{
  if(not option_locked()
     and ((now ^ option_loaded(written_word_)) & option_mask(written_word_)) != 0
     and not std::ranges::contains(option_changed_, written_word_))
  {
    option_changed_.push_back(written_word_);
  }
  if(option_words_.empty())
  {
    if(option_locked())
    {
      regs_.poke(written_word_, old);
    }
    return;
  }
  for(auto const [offset, value] : option_words_)
  {
    regs_.poke(offset, value);
  }
}

template <typename Traits>
void flash_model<Traits>::wrp_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(written_word_, option_locked() ? old : Traits::wrp_normal(now, ctx_.part.part));
}

template <typename Traits>
void flash_model<Traits>::staging_written(std::uint32_t old, std::uint32_t now)
{
  if(option_locked())
  {
    regs_.poke(written_word_, old);
    return;
  }
  if(now != old and not std::ranges::contains(option_changed_, written_word_))
  {
    option_changed_.push_back(written_word_);
  }
}

template <typename Traits>
auto flash_model<Traits>::wrp_programmed(std::size_t index) const -> std::uint32_t
{
  return state_.wrp[index];
}

template <typename Traits>
auto flash_model<Traits>::option_loaded(std::uint32_t offset) const -> std::uint32_t
{
  auto const found =
      std::ranges::find(option_words_, offset, &std::pair<std::uint32_t, std::uint32_t>::first);
  if(found != option_words_.end())
  {
    return found->second;
  }
  for(auto const name : Traits::option_registers)
  {
    if(has_register(name) and reg(name).offset == offset)
    {
      auto const& described = describe(name);
      return described.reset & described.reset_mask;
    }
  }
  return 0;
}

template <typename Traits>
auto flash_model<Traits>::option_mask(std::uint32_t offset) const -> std::uint32_t
{
  for(auto const name : Traits::option_registers)
  {
    if(has_register(name) and reg(name).offset == offset)
    {
      return Traits::option_mask(name, ctx_.part.part);
    }
  }
  return ~0u;
}

template <typename Traits> void flash_model<Traits>::warn_dropped_options()
{
  if(option_changed_.empty() or std::exchange(option_logged_, true))
  {
    return;
  }
  auto const offset = option_changed_.front();
  for(auto const list :
      {std::span<std::string_view const>{Traits::option_registers},
       std::span<std::string_view const>{Traits::option_staging_registers}})
  {
    for(auto const name : list)
    {
      if(has_register(name) and reg(name).offset == offset)
      {
        warn(
            name, std::format(
                      "written, but OPTSTRT programs only the {} registers: the machine's option "
                      "bytes set the others",
                      Traits::wrp_text));
        return;
      }
    }
  }
}

template <typename Traits>
auto flash_model<Traits>::wrp_protected(std::uint32_t from, std::uint32_t length) const -> bool
{
  if(storage_ == nullptr or wrp_offsets_.empty())
  {
    return false;
  }
  auto const [base, size] = banks();
  for(auto at = from; at - from < length; at += Traits::page_bytes)
  {
    if(at - base >= 2 * size)
    {
      continue;
    }
    if(Traits::wrp_covers(
           state_.wrp_active, bank_of(at), (at - bank_base(at)) / Traits::page_bytes,
           ctx_.part.part))
    {
      return true;
    }
  }
  return false;
}

template <typename Traits> auto flash_model<Traits>::loads_options(reset_kind kind) -> bool
{
  return kind == reset_kind::power_on
      or kind == reset_kind::brown_out
      or kind == reset_kind::option_load;
}

template <typename Traits> void flash_model<Traits>::poke_wrp()
{
  for(auto const [index, offset] : std::views::enumerate(wrp_active_offsets_))
  {
    regs_.poke(offset, state_.wrp_active[static_cast<std::size_t>(index)]);
  }
}

template <typename Traits> void flash_model<Traits>::load_options()
{
  for(auto const [index, offset] : std::views::enumerate(wrp_offsets_))
  {
    regs_.poke(offset, wrp_programmed(static_cast<std::size_t>(index)));
  }
}

template <typename Traits> void flash_model<Traits>::program_options()
{
  if(flag(false, set_role::sequence_error) or get(regs_, unit_[unit_role::option_error]) != 0)
  {
    load_options();
    return;
  }
  std::vector<std::uint32_t> requested;
  auto                       violation = false;
  for(auto const [index, offset] : std::views::enumerate(wrp_offsets_))
  {
    auto const programmed = wrp_programmed(static_cast<std::size_t>(index));
    auto const word       = Traits::wrp_normal(regs_.peek(offset), ctx_.part.part);
    requested.push_back(word);
    violation = violation or Traits::wrp_refused(programmed, word);
  }
  if(violation)
  {
    load_options();
    fail(false, unit_[unit_role::option_error]);
    return;
  }
  state_.wrp = std::move(requested);
  if constexpr(Traits::option_reload_on_start)
  {
    state_.wrp_active = state_.wrp;
    poke_wrp();
  }
}

template <typename Traits> auto flash_model<Traits>::option_locked() const -> bool
{
  auto const found = unit_.find(unit_role::option_lock);
  return found and get(regs_, *found) != 0;
}

template <typename Traits> void flash_model<Traits>::power_cut()
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
  auto const lines = state_.length / Traits::program_bytes;
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
  sync_watches();
}

template <typename Traits>
auto flash_model<Traits>::duration_of(operation which, std::uint32_t length) const
    -> std::chrono::nanoseconds
{
  constexpr auto const& timing = Traits::timing;
  switch(which)
  {
    case operation::program:
      return length == Traits::program_bytes
               ? options_.quad_word_program.value_or(timing.quad_word_program)
               : options_.burst_program.value_or(timing.burst_program);
    case operation::page_erase:
      return options_.page_erase.value_or(timing.page_erase);
    case operation::bank_erase:
      return options_.bank_erase.value_or(timing.bank_erase);
    case operation::mass_erase:
    case operation::none:
      break;
  }
  return options_.mass_erase.value_or(timing.mass_erase);
}

template <typename Traits>
auto flash_model<Traits>::refused(
    std::uint32_t              at,
    std::span<std::byte const> target,
    std::span<std::byte const> data) const -> bool
{
  if constexpr(Traits::force_write)
  {
    return false;
  }
  for(std::size_t line = 0; line < data.size(); line += Traits::program_bytes)
  {
    auto const old       = target.subspan(line, Traits::program_bytes);
    auto const zero_data = all_of_byte(data.subspan(line, Traits::program_bytes), std::byte{0});
    if(all_of_byte(old, std::byte{0}) or (not all_of_byte(old, std::byte{0xff}) and not zero_data))
    {
      return true;
    }
  }
  auto const first = std::ranges::lower_bound(state_.torn, at);
  return first != state_.torn.end() and *first < at + data.size();
}

template <typename Traits>
auto flash_model<Traits>::buffered() const -> std::array<std::byte, Traits::buffer_bytes>
{
  std::array<std::byte, Traits::buffer_bytes> data{};
  for(std::size_t i = 0; i < data.size(); ++i)
  {
    data[i] = static_cast<std::byte>(state_.buffer[i / 4] >> (8 * (i % 4)));
  }
  return data;
}

template <typename Traits> void flash_model<Traits>::tear_program(std::uint32_t full)
{
  std::array<std::byte, Traits::buffer_bytes> target_bytes{};
  auto const                                  target = std::span{target_bytes}.first(state_.length);
  auto                                        data_bytes = buffered();
  auto const                                  data = std::span{data_bytes}.first(state_.length);
  if(not ctx_.services.read_block(state_.target, target, initiator::debugger)
     or refused(state_.target, target, data))
  {
    return;
  }
  auto const done = full * Traits::program_bytes;
  if(done < data.size())
  {
    for(std::size_t i = done; i < done + Traits::program_bytes / 2; ++i)
    {
      data[i] &= target[i];
    }
    std::ranges::copy(
        target.subspan(done + Traits::program_bytes / 2),
        data.begin() + done + Traits::program_bytes / 2);
    auto const line = state_.target + done;
    state_.torn.insert(std::ranges::upper_bound(state_.torn, line), line);
  }
  static_cast<void>(ctx_.services.write_block(state_.target, data, initiator::debugger));
}

template <typename Traits> void flash_model<Traits>::tear_erase(std::uint32_t full)
{
  auto const                   done = full * Traits::program_bytes;
  std::vector<std::byte> const erased(done, std::byte{0xff});
  static_cast<void>(ctx_.services.write_block(state_.target, erased, initiator::debugger));
  forget_torn(state_.target, done);
  if(done >= state_.length)
  {
    return;
  }
  std::array<std::byte, Traits::program_bytes> old{};
  auto const                                   line = state_.target + done;
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

template <typename Traits>
void flash_model<Traits>::forget_torn(std::uint32_t from, std::uint32_t length)
{
  auto const first = std::ranges::lower_bound(state_.torn, from);
  auto const last  = std::ranges::lower_bound(state_.torn, from + length);
  state_.torn.erase(first, last);
}

template <typename Traits> void flash_model<Traits>::sync_watches()
{
  std::vector<std::uint32_t> gone;
  std::vector<std::uint32_t> added;
  std::ranges::set_difference(watched_, state_.torn, std::back_inserter(gone));
  std::ranges::set_difference(state_.torn, watched_, std::back_inserter(added));
  for(auto const line : gone)
  {
    ctx_.services.watch_reads(line, Traits::program_bytes, false);
  }
  for(auto const line : added)
  {
    ctx_.services.watch_reads(line, Traits::program_bytes, true);
  }
  watched_ = state_.torn;
}

template <typename Traits> auto flash_model<Traits>::banks() const -> bank_geometry
{
  contract_assert(storage_ != nullptr);
  return {.base = static_cast<std::uint32_t>(storage_->start), .size = storage_->size / 2};
}

template <typename Traits>
auto flash_model<Traits>::bank_of(std::uint32_t at) const -> std::uint32_t
{
  auto const [base, size] = banks();
  return ((at - base) / size) ^ (swapped_ ? 1u : 0u);
}

template <typename Traits>
auto flash_model<Traits>::bank_base(std::uint32_t at) const -> std::uint32_t
{
  auto const [base, size] = banks();
  return base + (at - base) / size * size;
}

template <typename Traits>
auto flash_model<Traits>::page_secure(address at) const -> std::optional<bool>
{
  return page_rule(at).transform([](page_policy const& page) { return page.secure; });
}

template <typename Traits>
auto flash_model<Traits>::page_rule(address at) const -> std::optional<page_policy>
{
  if(not ctx_.trustzone
     or storage_ == nullptr
     or option_words_.size() != Traits::option_registers.size())
  {
    return std::nullopt;
  }
  auto const region = std::ranges::find_if(ctx_.part.regions, [&](region_desc const& r) {
    return r.controller == desc_.name and at >= r.start and at - r.start < r.size;
  });
  if(region == ctx_.part.regions.end())
  {
    return std::nullopt;
  }
  return rule_of(physical(*region, at));
}

template <typename Traits> auto flash_model<Traits>::rule_of(std::uint32_t at) const -> page_policy
{
  page_policy policy{
      .secure = block_bit(secure_blocks_, at), .privileged = block_bit(privileged_blocks_, at)};
  if(option_words_.size() != Traits::option_registers.size())
  {
    return policy;
  }
  auto const bank        = bank_of(at);
  auto const page        = (at - bank_base(at)) / Traits::page_bytes;
  auto const first       = option_words_[Traits::watermark_words[bank]].second;
  auto const top         = Traits::watermark_top(ctx_.part.part);
  auto const watermarked = (first & top) <= page and page <= (first >> 16 & top);
  auto const hide        = unit_.find(bank == 0 ? unit_role::hide_bank1 : unit_role::hide_bank2);
  auto const extension =
      unit_.find(bank == 0 ? unit_role::hide_extension1 : unit_role::hide_extension2);
  auto const* const isolation = dynamic_cast<temporal_isolation const*>(ctx_.peers.find("SBS"));
  policy.secure               = policy.secure or watermarked;
  policy.hidden               = Traits::hidden(
      {.watermark       = first,
       .area            = option_words_[Traits::hide_words[bank]].second,
       .access_disabled = hide and get(regs_, *hide) != 0,
       .extension       = extension ? get(regs_, *extension) : 0,
       .level           = isolation != nullptr ? isolation->isolation_level() : std::uint8_t{0},
       .page            = page,
       .top             = top});
  return policy;
}

template <typename Traits>
auto flash_model<Traits>::block_bit(std::span<std::uint32_t const> words, std::uint32_t at) const
    -> bool
{
  auto const page = (at - bank_base(at)) / Traits::page_bytes;
  auto const index =
      bank_of(at) * Traits::block_registers_per_bank + page / pages_per_block_register;
  if(index >= words.size() or words[index] == ~0u)
  {
    return false;
  }
  return (regs_.peek(words[index]) >> (page % pages_per_block_register) & 1u) != 0;
}

template <typename Traits> auto flash_model<Traits>::secure_page_size() const -> std::uint32_t
{
  return Traits::page_bytes;
}

template <typename Traits> auto flash_model<Traits>::secure_function() const -> bool
{
  if(not ctx_.trustzone or storage_ == nullptr)
  {
    return false;
  }
  auto const [base, size] = banks();
  auto any                = false;
  for(std::uint32_t at = base; not any and at - base < 2 * size; at += Traits::page_bytes)
  {
    any = rule_of(at).secure;
  }
  return any != (get(regs_, unit_[unit_role::invert]) != 0);
}

template <typename Traits> auto flash_model<Traits>::secure_word() const -> std::uint32_t
{
  return secure_function() ? 1u : 0u;
}

template <typename Traits>
auto flash_model<Traits>::bank_start(std::uint32_t bank) const -> std::uint32_t
{
  auto const [base, size] = banks();
  return base + (bank ^ (swapped_ ? 1u : 0u)) * size;
}

template <typename Traits>
void flash_model<Traits>::on_memory_read(
    region_desc const& region,
    address            addr,
    std::uint8_t       size,
    access_ctx)
{
  auto const at    = physical(region, addr);
  auto const first = at & ~(Traits::program_bytes - 1);
  auto const last  = (at + size - 1) & ~(Traits::program_bytes - 1);
  for(auto line = first; line <= last; line += Traits::program_bytes)
  {
    if(std::ranges::binary_search(state_.torn, line))
    {
      report_double_error(line);
    }
  }
}

template <typename Traits> void flash_model<Traits>::report_double_error(std::uint32_t line)
{
  if(get(regs_, unit_[unit_role::ecc_detect]) != 0
     or get(regs_, unit_[unit_role::ecc_correct]) != 0)
  {
    return;
  }
  update(regs_, unit_[unit_role::ecc_address], Traits::ecc_address(line, bank_base(line)));
  update(regs_, unit_[unit_role::ecc_bank], bank_of(line));
  update(regs_, unit_[unit_role::ecc_detect], 1);
  ctx_.nmi.raise();
}

template <typename Traits> auto flash_model<Traits>::busy() const -> bool
{
  return static_cast<operation>(state_.running) != operation::none;
}

template <typename Traits>
auto flash_model<Traits>::field_of(bool secure, set_role role) const -> field_ref
{
  return sets_[secure ? 1 : 0].fields[role];
}

template <typename Traits>
auto flash_model<Traits>::value_of(bool secure, set_role role, std::uint32_t word) const
    -> std::uint32_t
{
  auto const found = sets_[secure ? 1 : 0].fields.find(role);
  return found ? found->of(word) : 0;
}

template <typename Traits>
auto flash_model<Traits>::unit_value(unit_role role) const -> std::uint32_t
{
  auto const found = unit_.find(role);
  return found ? get(regs_, *found) : 0;
}

template <typename Traits> auto flash_model<Traits>::flag(bool secure, set_role role) const -> bool
{
  auto const found = sets_[secure ? 1 : 0].fields.find(role);
  return found and get(regs_, *found) != 0;
}

template <typename Traits> void flash_model<Traits>::set_busy(bool on)
{
  update(regs_, field_of(false, set_role::busy), on ? 1 : 0);
  if(ctx_.trustzone)
  {
    update(regs_, field_of(true, set_role::busy), on ? 1 : 0);
  }
}

template <typename Traits> void flash_model<Traits>::set_waiting(bool on)
{
  update(regs_, field_of(false, set_role::buffer_open), on ? 1 : 0);
  if(ctx_.trustzone)
  {
    update(regs_, field_of(true, set_role::buffer_open), on ? 1 : 0);
  }
}

template <typename Traits> auto flash_model<Traits>::errors_pending(bool secure) const -> bool
{
  return std::ranges::any_of(error_roles, [&](set_role role) { return flag(secure, role); })
      or (not secure and get(regs_, unit_[unit_role::option_error]) != 0);
}

template <typename Traits> void flash_model<Traits>::interrupt()
{
  auto const raised = [this](bool secure) {
    auto const option_error = not secure
                          and unit_value(unit_role::option_error) != 0
                          and unit_value(unit_role::option_error_irq) != 0;
    return (flag(secure, set_role::end) and flag(secure, set_role::end_irq))
        or (flag(secure, set_role::operation_error) and flag(secure, set_role::error_irq))
        or std::ranges::any_of(
               Traits::error_enables,
               [&](error_enable const& pair) {
                 return flag(secure, pair.flag) and flag(secure, pair.enable);
               })
        or option_error;
  };
  drive(line_, raised(false));
  if(secure_line_ != nullptr)
  {
    drive(*secure_line_, raised(true));
  }
}

template <typename Traits> void flash_model<Traits>::fail(bool secure, set_role role)
{
  fail(secure, field_of(secure, role));
}

template <typename Traits> void flash_model<Traits>::fail(bool secure, field_ref flagged)
{
  update(regs_, flagged, 1);
  if(flag(secure, set_role::error_irq))
  {
    update(regs_, field_of(secure, set_role::operation_error), 1);
  }
  auto const owner = secure == secure_.running_secure;
  if(not busy() or owner)
  {
    state_.filled = 0;
    set_waiting(false);
  }
  if(busy() and owner)
  {
    state_.running = static_cast<std::uint8_t>(operation::none);
    state_.held.clear();
    secure_.held_attrs.clear();
    set_busy(false);
    update(regs_, field_of(secure_.running_secure, set_role::start), 0);
  }
  interrupt();
}

template <typename Traits> void flash_model<Traits>::hold_hsi16(bool held)
{
  for(auto* const peer : ctx_.peers.all())
  {
    if(auto* const source = dynamic_cast<hsi16_source*>(peer))
    {
      source->hold_hsi16(held);
    }
  }
}

template <typename Traits> void flash_model<Traits>::illegal_access(std::string_view enable_field)
{
  if(not events_linked_ and not ctx_.peers.all().empty())
  {
    events_linked_ = true;
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const tzic = dynamic_cast<illegal_access_controller*>(peer))
      {
        if(tzic->source("FLASHIE") and tzic->source("FLASH_REGIE"))
        {
          events_ = tzic;
          break;
        }
      }
    }
  }
  if(events_ != nullptr)
  {
    if(auto const source = events_->source(enable_field))
    {
      events_->illegal_access(*source);
    }
  }
}

template <typename Traits>
auto flash_model<Traits>::register_refused(std::uint32_t word, access_ctx ctx, bool write) -> bool
{
  auto const in = [word](std::span<std::uint32_t const> words) {
    return std::ranges::contains(words, word);
  };
  if(not ctx_.trustzone)
  {
    return in(secure_words_) or in(secure_blocks_);
  }
  if(in(secure_words_))
  {
    if(not ctx.secure)
    {
      if(ctx.by != initiator::debugger)
      {
        illegal_access("FLASH_REGIE");
      }
      return true;
    }
    return not ctx.privileged and get(regs_, (*privilege_)[privilege_role::secure]) != 0;
  }
  if(in(secure_blocks_))
  {
    if(write and not ctx.secure)
    {
      if(ctx.by != initiator::debugger)
      {
        illegal_access("FLASH_REGIE");
      }
      return true;
    }
    return false;
  }
  if(in(privileged_blocks_))
  {
    return write and not ctx.privileged;
  }
  if(word == privilege_word_)
  {
    return false;
  }
  return not ctx.privileged and get(regs_, (*privilege_)[privilege_role::nonsecure]) != 0;
}

template <typename Traits>
auto flash_model<Traits>::writable(
    std::uint32_t from,
    std::uint32_t length,
    bool          secure,
    bool          privileged) -> bool
{
  auto allowed = true;
  auto event   = false;
  for(auto at = from; at - from < length; at += Traits::page_bytes)
  {
    auto const rule = rule_of(at);
    event           = event or rule.secure != secure;
    allowed         = allowed
          and rule.secure == secure
          and not rule.hidden
          and (privileged or not rule.privileged);
  }
  if(event)
  {
    illegal_access("FLASHIE");
  }
  return allowed;
}

template <typename Traits>
void flash_model<Traits>::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != operation_end or payload != state_.generation or not busy())
  {
    return;
  }
  finish();
}

template <typename Traits>
auto flash_model<Traits>::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  written_      = merge_write(0, offset, size, value);
  written_word_ = offset & ~3u;
  access_       = ctx;
  if(register_refused(written_word_, ctx, true))
  {
    return {};
  }
  auto const done = base::on_write(regs, offset, size, old, value, ctx);
  if(std::exchange(rejected_, false))
  {
    return std::unexpected{bus_error::denied};
  }
  return done;
}

template <typename Traits>
auto flash_model<Traits>::on_memory_write(
    region_desc const& region,
    address            addr,
    std::uint8_t       size,
    std::uint32_t      value,
    access_ctx         ctx) -> std::expected<void, bus_error>
{
  auto const       secure = ctx_.trustzone and ctx.secure;
  held_store const one{
      .addr    = physical(region, addr),
      .size    = size,
      .value   = value,
      .control = regs_.peek(sets_[secure ? 1 : 0].control.offset),
      .dma     = ctx.by == initiator::dma};
  auto const attrs = static_cast<std::uint8_t>(
      (secure ? held_secure : 0u) | (ctx.privileged ? held_privileged : 0u));
  if(field_of(secure, set_role::program).of(one.control) != 0)
  {
    check_programming_delay("on a flash store with PG set");
  }
  if(busy())
  {
    hold(one, attrs);
    return {};
  }
  store(one, attrs);
  return {};
}

template <typename Traits> void flash_model<Traits>::hold(held_store const& one, std::uint8_t attrs)
{
  state_.held.push_back(one);
  if(ctx_.trustzone)
  {
    secure_.held_attrs.push_back(attrs);
  }
}

template <typename Traits>
void flash_model<Traits>::store(held_store const& one, std::uint8_t attrs)
{
  auto const [addr, size, value, written, dma] = one;
  auto const secure                            = (attrs & held_secure) != 0;
  if((ctx_.trustzone and not writable(addr, 1, secure, (attrs & held_privileged) != 0))
     or wrp_protected(addr, 1))
  {
    fail(secure, set_role::protection_error);
    return;
  }
  auto const control = dma ? regs_.peek(sets_[secure ? 1 : 0].control.offset) : written;
  if(field_of(secure, set_role::program).of(control) == 0)
  {
    fail(secure, set_role::sequence_error);
    return;
  }
  if(state_.filled == 0 and errors_pending(secure))
  {
    fail(secure, set_role::sequence_error);
    return;
  }
  if(state_.filled != 0 and secure_.buffer_secure != secure)
  {
    fail(secure, Traits::force_write ? set_role::inconsistency_error : set_role::sequence_error);
    return;
  }
  if constexpr(Traits::force_write)
  {
    if(Traits::erase_selected([&](set_role role) { return value_of(secure, role, control); }))
    {
      fail(secure, set_role::sequence_error);
      return;
    }
    fill(addr, size, value, secure);
    return;
  }
  if(size != 4)
  {
    fail(secure, set_role::size_error);
    return;
  }
  auto const next =
      state_.buffer_addr + 4 * static_cast<std::uint32_t>(std::popcount(state_.filled));
  if(state_.filled == 0)
  {
    state_.burst = field_of(secure, set_role::burst).of(control) != 0;
    if(addr % (state_.burst ? Traits::buffer_bytes : Traits::program_bytes) != 0)
    {
      fail(secure, set_role::alignment_error);
      return;
    }
    state_.buffer_addr    = addr;
    secure_.buffer_secure = secure;
    set_waiting(true);
  }
  else if(
      state_.burst
          ? addr != next
          : addr < state_.buffer_addr or addr >= state_.buffer_addr + Traits::program_bytes)
  {
    fail(secure, set_role::alignment_error);
    return;
  }
  auto const length   = state_.burst ? Traits::buffer_bytes : Traits::program_bytes;
  auto const word     = (addr - state_.buffer_addr) / 4;
  state_.buffer[word] = value;
  state_.filled |= 1u << word;
  if(std::cmp_not_equal(std::popcount(state_.filled), length / 4))
  {
    return;
  }
  set_waiting(false);
  start(operation::program, state_.buffer_addr, length, secure);
}

template <typename Traits>
void flash_model<Traits>::fill(
    std::uint32_t addr,
    std::uint8_t  size,
    std::uint32_t value,
    bool          secure)
{
  auto const line  = addr & ~(Traits::program_bytes - 1);
  auto const first = addr - line;
  if(state_.filled == 0)
  {
    state_.buffer_addr    = line;
    secure_.buffer_secure = secure;
    std::ranges::fill(state_.buffer, ~0u);
    set_waiting(true);
  }
  else if(line != state_.buffer_addr)
  {
    fail(secure, set_role::inconsistency_error);
    return;
  }
  if(first + size > Traits::program_bytes)
  {
    fail(secure, set_role::inconsistency_error);
    return;
  }
  auto const bytes = ((1u << size) - 1) << first;
  if((state_.filled & bytes) != 0)
  {
    fail(secure, set_role::strobe_error);
    return;
  }
  for(std::uint32_t i = 0; i < size; ++i)
  {
    auto const at   = first + i;
    auto&      out  = state_.buffer[at / 4];
    auto const lane = 8 * (at % 4);
    out             = (out & ~(0xffu << lane)) | ((value >> (8 * i) & 0xffu) << lane);
  }
  state_.filled |= bytes;
  if(state_.filled != (1u << Traits::program_bytes) - 1)
  {
    return;
  }
  set_waiting(false);
  start(operation::program, state_.buffer_addr, Traits::program_bytes, secure);
}

template <typename Traits> void flash_model<Traits>::force(bool secure)
{
  if(state_.filled == 0 or busy() or secure_.buffer_secure != secure)
  {
    return;
  }
  auto const data = buffered();
  if(all_of_byte(std::span{data}.first(Traits::program_bytes), std::byte{0xff}))
  {
    return;
  }
  set_waiting(false);
  start(operation::program, state_.buffer_addr, Traits::program_bytes, secure);
}

template <typename Traits>
void flash_model<Traits>::start(
    operation     which,
    std::uint32_t target,
    std::uint32_t length,
    bool          secure)
{
  state_.running         = static_cast<std::uint8_t>(which);
  secure_.running_secure = secure;
  state_.target          = target;
  state_.length          = length;
  set_busy(true);
  auto const delay = duration_of(which, length);
  state_.due_ps =
      ctx_.clock.since_power_on().ps + static_cast<std::uint64_t>(delay.count()) * 1000u;
  schedule_in(delay, operation_end, ++state_.generation);
}

template <typename Traits> void flash_model<Traits>::start_erase(bool secure, std::uint32_t control)
{
  auto const register_name = Traits::set_registers[secure ? 1 : 0].control;
  check_programming_delay("on an erase START");
  if(storage_ == nullptr)
  {
    warn(register_name, "erase requested, but no flash region names FLASH as its controller");
    return;
  }
  auto const [base, bank_size] = banks();
  auto const field             = [&](set_role role) { return value_of(secure, role, control); };
  auto const command           = Traits::decode_erase(field);
  auto       target            = base;
  auto       length            = storage_->size;
  if(command.op == operation::page_erase)
  {
    auto const pages = bank_size / Traits::page_bytes;
    auto const page  = command.page;
    if(page >= pages)
    {
      update(regs_, field_of(secure, set_role::start), 0);
      if(not std::exchange(page_logged_, true))
      {
        warn(
            register_name, std::format(
                               "{} {} is past the {} pages of a bank: nothing is erased",
                               Traits::page_field, page, pages));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, std::string{register_name}, reg(register_name).offset);
      }
      return;
    }
    target = bank_start(command.bank) + page * Traits::page_bytes;
    length = Traits::page_bytes;
  }
  else if(command.op == operation::bank_erase)
  {
    target = bank_start(command.bank);
    length = bank_size;
  }
  if((ctx_.trustzone and not writable(target, length, secure, access_.privileged))
     or wrp_protected(target, length))
  {
    fail(secure, set_role::protection_error);
    update(regs_, field_of(secure, set_role::start), 0);
    return;
  }
  start(command.op, target, length, secure);
}

template <typename Traits> void flash_model<Traits>::program(bool secure)
{
  std::array<std::byte, Traits::buffer_bytes> target_bytes{};
  auto const                                  target = std::span{target_bytes}.first(state_.length);
  auto const                                  data_bytes = buffered();
  auto const                                  data = std::span{data_bytes}.first(state_.length);
  state_.filled                                    = 0;
  if(not ctx_.services.read_block(state_.target, target, initiator::debugger)
     or refused(state_.target, target, data))
  {
    fail(secure, set_role::program_error);
    return;
  }
  static_cast<void>(ctx_.services.write_block(state_.target, data, initiator::debugger));
}

template <typename Traits> void flash_model<Traits>::finish()
{
  auto const which  = static_cast<operation>(state_.running);
  auto const secure = secure_.running_secure;
  state_.running    = static_cast<std::uint8_t>(operation::none);
  set_busy(false);
  auto const errors = errors_pending(secure);
  if(which == operation::program)
  {
    program(secure);
  }
  else
  {
    std::vector<std::byte> const erased(state_.length, std::byte{0xff});
    static_cast<void>(ctx_.services.write_block(state_.target, erased, initiator::debugger));
    forget_torn(state_.target, state_.length);
    sync_watches();
    update(regs_, field_of(secure, set_role::start), 0);
  }
  if(errors_pending(secure) == errors
     and (Traits::end_flag_always or flag(secure, set_role::end_irq)))
  {
    update(regs_, field_of(secure, set_role::end), 1);
  }
  interrupt();
  auto held  = std::exchange(state_.held, {});
  auto attrs = std::exchange(secure_.held_attrs, {});
  for(auto const [index, one] : std::views::enumerate(held))
  {
    auto const attr =
        index < std::ssize(attrs) ? attrs[static_cast<std::size_t>(index)] : held_privileged;
    if(busy())
    {
      hold(one, attr);
      continue;
    }
    store(one, attr);
  }
  if(not busy() and not hsi16_held())
  {
    hold_hsi16(false);
  }
}

template <typename Traits> auto flash_model<Traits>::hsi16_held() const -> bool
{
  if constexpr(not Traits::holds_hsi16)
  {
    return false;
  }
  return busy() or std::ranges::any_of(std::array{false, true}, [this](bool secure) {
           return (secure ? ctx_.trustzone : true)
              and (flag(secure, set_role::program) or flag(secure, set_role::start));
         });
}

template <typename Traits> auto flash_model<Traits>::active_range() const -> std::uint32_t
{
  std::uint32_t range = 1;
  for(auto* const peer : ctx_.peers.all())
  {
    if(auto const* const source = dynamic_cast<voltage_range_source const*>(peer))
    {
      range = source->active_level() + 1;
    }
  }
  return range;
}

template <typename Traits>
void flash_model<Traits>::check_programming_delay(std::string_view trigger)
{
  auto const field = unit_.find(unit_role::programming_delay);
  if(not field)
  {
    return;
  }
  auto const hclk = ctx_.clock.hz(hclk_);
  if(hclk == 0)
  {
    return;
  }
  auto const range    = active_range();
  auto const required = Traits::programming_delay(hclk, range);
  auto const delay    = get(regs_, *field);
  if(not required or delay >= *required)
  {
    return;
  }
  if(delay_logged_.emplace(hclk, range, delay).second or ctx_.strict_peripherals)
  {
    warn(
        "FLASH_ACR", std::format(
                         "{}: WRHIGHFREQ {} is below the {} {} asks for HCLK {} Hz in voltage "
                         "range {}, silicon would program out of timing",
                         trigger, delay, *required, Traits::latency_section, hclk, range));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, "FLASH_ACR", reg("FLASH_ACR").offset);
  }
}

template <typename Traits> void flash_model<Traits>::check_latency(std::string_view trigger)
{
  auto const hclk = ctx_.clock.hz(hclk_);
  if(hclk == 0)
  {
    return;
  }
  auto const range   = active_range();
  auto const latency = get(regs_, unit_[unit_role::latency]);
  auto const required =
      Traits::wait_states(hclk, range, unit_value(unit_role::low_power_read) != 0);
  if(required and latency >= *required)
  {
    return;
  }
  if(latency_logged_.emplace(hclk, range, latency).second or ctx_.strict_peripherals)
  {
    warn(
        "FLASH_ACR",
        required ? std::format(
                       "{}: LATENCY {} is below the {} wait states {} asks for HCLK {} Hz in "
                       "voltage range {}, silicon would read wrong data",
                       trigger, latency, *required, Traits::latency_section, hclk, range)
                 : std::format(
                       "{}: HCLK {} Hz is above the maximum of voltage range {}, {}", trigger, hclk,
                       range, Traits::latency_section));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, "FLASH_ACR", reg("FLASH_ACR").offset);
  }
}

template <typename Traits> void flash_model<Traits>::save(std::vector<std::byte>& out) const
{
  if(ctx_.trustzone)
  {
    reflex::serde::bson::serializer{out}.dump(
        trustzone_flash_state{.flash = state_, .secure = secure_});
    return;
  }
  base::save(out);
}

template <typename Traits> void flash_model<Traits>::load(std::span<std::byte const> in)
{
  if(ctx_.trustzone)
  {
    auto loaded = reflex::serde::bson::deserializer{in}.load<trustzone_flash_state>();
    state_      = std::move(loaded.flash);
    secure_     = std::move(loaded.secure);
    ctx_.services.memory_policy_changed();
  }
  else
  {
    base::load(in);
  }
  option_changed_.clear();
  latency_logged_.clear();
  delay_logged_.clear();
  sync_watches();
}

template <typename Traits> void flash_model<Traits>::on_clock_change()
{
  check_latency("after an HCLK change");
}

template <typename Traits> void flash_model<Traits>::on_voltage_range_change()
{
  check_latency("after a voltage range change");
}

template <typename Traits> void flash_model<Traits>::access_written(std::uint32_t, std::uint32_t)
{
  check_latency("on a FLASH_ACR write");
}

template <typename Traits> void flash_model<Traits>::key_written(std::uint32_t, std::uint32_t now)
{
  key_written(false, now);
}

template <typename Traits>
void flash_model<Traits>::secure_key_written(std::uint32_t, std::uint32_t now)
{
  key_written(true, now);
}

template <typename Traits> void flash_model<Traits>::key_written(bool secure, std::uint32_t now)
{
  auto& first_key = secure ? secure_.first_key : state_.first_key;
  auto& locked_up = secure ? secure_.locked_up : state_.locked_up;
  regs_.poke(sets_[secure ? 1 : 0].key.offset, 0);
  auto const locked = flag(secure, set_role::lock);
  if(not locked_up and locked and not first_key and now == key1)
  {
    first_key = true;
    return;
  }
  if(not locked_up and locked and first_key and now == key2)
  {
    first_key = false;
    update(regs_, field_of(secure, set_role::lock), 0);
    return;
  }
  first_key = false;
  locked_up = true;
  rejected_ = true;
}

template <typename Traits>
void flash_model<Traits>::option_key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(reg("FLASH_OPTKEYR").offset, 0);
  auto const armed = option_locked()
                 and not(Traits::option_key_needs_unlock and flag(false, set_role::lock))
                 and not state_.option_locked_up;
  if(armed and not state_.option_first_key and now == option_key1)
  {
    state_.option_first_key = true;
    return;
  }
  if(armed and state_.option_first_key and now == option_key2)
  {
    state_.option_first_key = false;
    update(regs_, unit_[unit_role::option_lock], 0);
    return;
  }
  state_.option_first_key = false;
  state_.option_locked_up = true;
  rejected_               = true;
}

template <typename Traits>
void flash_model<Traits>::control_written(std::uint32_t old, std::uint32_t now)
{
  control_written(false, old, now);
}

template <typename Traits>
void flash_model<Traits>::secure_control_written(std::uint32_t old, std::uint32_t now)
{
  control_written(true, old, now);
}

template <typename Traits>
void flash_model<Traits>::control_written(bool secure, std::uint32_t old, std::uint32_t now)
{
  auto const of = [&](set_role role, std::uint32_t word) {
    return value_of(secure, role, word) != 0;
  };
  auto const in = [&](std::uint32_t word) {
    return [&, word](set_role role) { return value_of(secure, role, word); };
  };
  auto const control = sets_[secure ? 1 : 0].control.offset;
  if(of(set_role::lock, old))
  {
    regs_.poke(control, old);
    return;
  }
  auto const option_lock       = option_field(unit_role::option_lock, control);
  auto const option_start_bit  = option_field(unit_role::option_start, control);
  auto const option_load_field = option_field(unit_role::option_load, control);
  auto const erase_select = [&](std::uint32_t word) { return Traits::erase_selected(in(word)); };
  auto const other_held =
      ctx_.trustzone and (flag(not secure, set_role::program) or flag(not secure, set_role::start));
  auto const hsi16_before = of(set_role::program, old) or of(set_role::start, old) or other_held;
  auto const program_set  = of(set_role::program, now) and not of(set_role::program, old);
  auto const start_set    = of(set_role::start, now) and not of(set_role::start, old);
  auto const unlocked     = option_lock and option_lock->of(old) == 0;
  auto const option_start = unlocked and option_start_bit and option_start_bit->of(now) != 0;
  auto const waiting      = flag(false, set_role::buffer_open);
  auto       sequence     = false;
  if constexpr(Traits::force_write)
  {
    if(start_set and of(set_role::program, now))
    {
      sequence = true;
    }
  }
  else if(
      of(set_role::program, now) and erase_select(now) and (program_set or not erase_select(old)))
  {
    sequence = true;
  }
  if(program_set and of(set_role::start, old))
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
  if(ctx_.trustzone and not secure and start_set and access_.secure)
  {
    sequence = true;
  }
  if(option_lock)
  {
    auto const relock =
        option_lock->of(old) != 0 or option_lock->of(now) != 0 or of(set_role::lock, now);
    now = option_lock->with(now, relock ? 1 : 0);
  }
  if(option_start_bit)
  {
    now = option_start_bit->with(now, 0);
  }
  if(option_load_field)
  {
    if(not unlocked)
    {
      now = option_load_field->with(now, option_load_field->of(old));
    }
  }
  auto const start = field_of(secure, set_role::start);
  now              = start.with(now, start.of(old));
  auto launch      = false;
  auto forced      = false;
  if(auto const fw = sets_[secure ? 1 : 0].fields.find(set_role::force_write))
  {
    forced = fw->of(now) != 0;
    now    = fw->with(now, 0);
  }
  if(start_set and not busy())
  {
    if(errors_pending(secure) or not erase_select(now) or Traits::erase_conflict(in(now)))
    {
      sequence = true;
    }
    if(not sequence)
    {
      now    = start.with(now, 1);
      launch = true;
    }
  }
  auto const load = unlocked and option_load_field and option_load_field->of(now) != 0;
  if(load)
  {
    now = option_load_field->with(now, 0);
  }
  regs_.poke(control, now);
  if(option_start and not sequence and not busy())
  {
    program_options();
    warn_dropped_options();
  }
  if(load)
  {
    ctx_.services.request_reset(reset_kind::option_load);
  }
  if(sequence)
  {
    fail(secure, set_role::sequence_error);
  }
  if(launch)
  {
    start_erase(secure, now);
    if(not busy())
    {
      update(regs_, start, 0);
    }
  }
  if(forced)
  {
    force(secure);
  }
  auto const hsi16_after = hsi16_held();
  if(hsi16_after != hsi16_before)
  {
    hold_hsi16(hsi16_after);
  }
  interrupt();
}

template <typename Traits>
void flash_model<Traits>::status_written(std::uint32_t old, std::uint32_t)
{
  status_written(false, old);
}

template <typename Traits>
void flash_model<Traits>::secure_status_written(std::uint32_t old, std::uint32_t)
{
  status_written(true, old);
}

template <typename Traits> void flash_model<Traits>::status_written(bool secure, std::uint32_t old)
{
  auto const& set = sets_[secure ? 1 : 0];
  regs_.poke(set.status.offset, old & ~(written_ & set.clearable));
  interrupt();
}

template <typename Traits> void flash_model<Traits>::clear_written(std::uint32_t, std::uint32_t now)
{
  apply_crosses(sets_[0].clears, now);
  regs_.poke(written_word_, 0);
  interrupt();
}

template <typename Traits>
void flash_model<Traits>::secure_clear_written(std::uint32_t, std::uint32_t now)
{
  apply_crosses(sets_.back().clears, now);
  regs_.poke(written_word_, 0);
  interrupt();
}

template <typename Traits>
void flash_model<Traits>::option_control_written(std::uint32_t old, std::uint32_t now)
{
  auto const lock      = unit_[unit_role::option_lock];
  auto const start     = unit_[unit_role::option_start];
  auto const unlocked  = lock.of(old) == 0;
  auto const requested = unlocked and start.of(now) != 0;
  auto       kept      = unlocked ? now : old;
  kept                 = lock.with(kept, lock.of(old) | lock.of(now));
  kept                 = start.with(kept, 0);
  regs_.poke(written_word_, kept);
  if(requested and not busy())
  {
    if(flag(false, set_role::buffer_open) or errors_pending(false))
    {
      fail(false, set_role::sequence_error);
      return;
    }
    program_options();
    warn_dropped_options();
  }
  interrupt();
}

template <typename Traits>
void flash_model<Traits>::secure_block_written(std::uint32_t old, std::uint32_t now)
{
  auto const index   = std::ranges::find(secure_blocks_, written_word_) - secure_blocks_.begin();
  auto const partner = privileged_blocks_[static_cast<std::size_t>(index)];
  auto const allowed = access_.privileged or partner == ~0u ? ~0u : ~regs_.peek(partner);
  auto const kept    = (now & allowed) | (old & ~allowed);
  regs_.poke(written_word_, kept);
  policy_written(old, kept);
}

template <typename Traits>
void flash_model<Traits>::hide_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(written_word_, old | now);
  policy_written(old, old | now);
}

template <typename Traits>
void flash_model<Traits>::extension_written(std::uint32_t old, std::uint32_t now)
{
  auto const* const isolation = dynamic_cast<temporal_isolation const*>(ctx_.peers.find("SBS"));
  auto              kept      = old;
  if(isolation == nullptr or isolation->isolation_level() <= 2)
  {
    for(auto const role : {unit_role::hide_extension1, unit_role::hide_extension2})
    {
      auto const bits = unit_[role];
      kept            = bits.with(kept, std::max(bits.of(old), bits.of(now)));
    }
  }
  regs_.poke(written_word_, kept);
  policy_written(old, kept);
}

template <typename Traits>
void flash_model<Traits>::privilege_written(std::uint32_t old, std::uint32_t now)
{
  auto allowed = 0u;
  if(access_.privileged)
  {
    allowed |= (*privilege_)[privilege_role::nonsecure].mask();
    if(access_.secure)
    {
      allowed |= (*privilege_)[privilege_role::secure].mask();
    }
  }
  regs_.poke(written_word_, (now & allowed) | (old & ~allowed));
}

template <typename Traits>
void flash_model<Traits>::privileged_block_written(std::uint32_t old, std::uint32_t now)
{
  auto allowed = ~0u;
  if(not access_.secure)
  {
    auto const index =
        std::ranges::find(privileged_blocks_, written_word_) - privileged_blocks_.begin();
    auto const bank  = static_cast<std::uint32_t>(index) / Traits::block_registers_per_bank;
    auto const first = static_cast<std::uint32_t>(index)
                     % Traits::block_registers_per_bank
                     * pages_per_block_register;
    for(std::uint32_t bit = 0; bit < pages_per_block_register; ++bit)
    {
      auto const at = bank_start(bank) + (first + bit) * Traits::page_bytes;
      if(first + bit >= banks().size / Traits::page_bytes or rule_of(at).secure)
      {
        allowed &= ~(1u << bit);
      }
    }
  }
  auto const kept = (now & allowed) | (old & ~allowed);
  regs_.poke(written_word_, kept);
  policy_written(old, kept);
}

template <typename Traits>
void flash_model<Traits>::policy_written(std::uint32_t old, std::uint32_t now)
{
  if(old != now)
  {
    ctx_.services.memory_policy_changed();
    ctx_.services.end_after_instruction();
  }
}

template <typename Traits>
void flash_model<Traits>::ecc_written(std::uint32_t old, std::uint32_t now)
{
  auto const detect = unit_[unit_role::ecc_detect];
  auto       flags  = 0u;
  for(auto const role : {unit_role::ecc_detect, unit_role::ecc_correct})
  {
    if(auto const found = unit_.find(role); found and found->offset == written_word_)
    {
      flags |= found->mask();
    }
  }
  regs_.poke(written_word_, (now & ~flags) | (old & flags & ~written_));
  if(get(regs_, detect) != 0)
  {
    return;
  }
  ctx_.nmi.lower();
  if constexpr(Traits::ecc_record_clears)
  {
    update(regs_, unit_[unit_role::ecc_address], 0);
    update(regs_, unit_[unit_role::ecc_bank], 0);
  }
}
}

auto check_option_bytes(part_description const& part, option_bytes const& wanted)
    -> std::expected<address, option_refusal>
{
  auto const unit    = std::ranges::find_if(part.peripherals, [](peripheral_desc const& candidate) {
    return candidate.ip == "flash" and not candidate.alias_of;
  });
  auto const storage = unit == part.peripherals.end()
                         ? part.regions.end()
                         : std::ranges::find_if(part.regions, [&unit](region_desc const& region) {
                             return region.controller == unit->name and not region.physical;
                           });
  auto const boot    = [](auto const& words) { return words.boot; };
  if(storage != part.regions.end() and unit->ip_version == u5_ip_version)
  {
    return u5_flash::encode_options(part.part, storage->size, wanted).transform(boot);
  }
  if(storage != part.regions.end() and unit->ip_version == h5_ip_version)
  {
    return h5_flash::encode_options(part.part, storage->size, wanted).transform(boot);
  }
  return std::unexpected{
      option_refusal{
                     .key      = "option_bytes",
                     .expected = std::format(
              "an STM32U5 or STM32H5 part with a flash region its FLASH controls, not {}", part.part)}
  };
}

auto flash_factory(flash_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<flash_model<u5_flash>>(ctx, desc, options);
  };
}

auto flash_h5_factory(flash_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<flash_model<h5_flash>>(ctx, desc, options);
  };
}
}
