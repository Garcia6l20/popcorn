#include "factories.hpp"
#include "family_names.hpp"
#include "gtzc.hpp"
#include "hsi16_source.hpp"
#include "model_base.hpp"
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
  bool                                   option_first_key{};
  bool                                   option_locked_up{};
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
  std::vector<std::uint32_t>             wrp{};
  std::vector<std::uint32_t>             wrp_active{};

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
  option_lock,
  start,
  option_start,
  option_load,
  program,
  page_erase,
  bank1_erase,
  bank2_erase,
  page,
  bank,
  burst,
  end_irq,
  error_irq,
  end,
  operation_error,
  program_error,
  protection_error,
  alignment_error,
  size_error,
  sequence_error,
  option_error,
  busy,
  waiting,
};

constexpr std::array<named_field<set_role>, 24> u5_set_fields{
    {
     {set_role::lock, {"CR", "LOCK"}},
     {set_role::option_lock, {"CR", "OPTLOCK", true}},
     {set_role::start, {"CR", "STRT"}},
     {set_role::option_start, {"CR", "OPTSTRT", true}},
     {set_role::option_load, {"CR", "OBL_LAUNCH", true}},
     {set_role::program, {"CR", "PG"}},
     {set_role::page_erase, {"CR", "PER"}},
     {set_role::bank1_erase, {"CR", "MER1"}},
     {set_role::bank2_erase, {"CR", "MER2"}},
     {set_role::page, {"CR", "PNB"}},
     {set_role::bank, {"CR", "BKER"}},
     {set_role::burst, {"CR", "BWR"}},
     {set_role::end_irq, {"CR", "EOPIE"}},
     {set_role::error_irq, {"CR", "ERRIE"}},
     {set_role::end, {"SR", "EOP"}},
     {set_role::operation_error, {"SR", "OPERR"}},
     {set_role::program_error, {"SR", "PROGERR"}},
     {set_role::protection_error, {"SR", "WRPERR"}},
     {set_role::alignment_error, {"SR", "PGAERR"}},
     {set_role::size_error, {"SR", "SIZERR"}},
     {set_role::sequence_error, {"SR", "PGSERR"}},
     {set_role::option_error, {"SR", "OPTWERR", true}},
     {set_role::busy, {"SR", "BSY"}},
     {set_role::waiting, {"SR", "WDW"}},
     }
};
static_assert(names_every_role(u5_set_fields));

constexpr std::array<set_role, 6> error_roles{
    set_role::program_error,    set_role::size_error,     set_role::alignment_error,
    set_role::protection_error, set_role::sequence_error, set_role::option_error,
};

constexpr std::array<std::string_view, 16> secure_block_registers{
    "FLASH_SEC1BBR1", "FLASH_SEC1BBR2", "FLASH_SEC1BBR3", "FLASH_SEC1BBR4",
    "FLASH_SEC1BBR5", "FLASH_SEC1BBR6", "FLASH_SEC1BBR7", "FLASH_SEC1BBR8",
    "FLASH_SEC2BBR1", "FLASH_SEC2BBR2", "FLASH_SEC2BBR3", "FLASH_SEC2BBR4",
    "FLASH_SEC2BBR5", "FLASH_SEC2BBR6", "FLASH_SEC2BBR7", "FLASH_SEC2BBR8",
};

constexpr std::array<std::string_view, 16> privileged_block_registers{
    "FLASH_PRIV1BBR1", "FLASH_PRIV1BBR2", "FLASH_PRIV1BBR3", "FLASH_PRIV1BBR4",
    "FLASH_PRIV1BBR5", "FLASH_PRIV1BBR6", "FLASH_PRIV1BBR7", "FLASH_PRIV1BBR8",
    "FLASH_PRIV2BBR1", "FLASH_PRIV2BBR2", "FLASH_PRIV2BBR3", "FLASH_PRIV2BBR4",
    "FLASH_PRIV2BBR5", "FLASH_PRIV2BBR6", "FLASH_PRIV2BBR7", "FLASH_PRIV2BBR8",
};

constexpr std::size_t block_registers_per_bank = 8;
constexpr std::size_t pages_per_block_register = 32;

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

struct option_words
{
  std::uint32_t optr{};
  std::uint32_t nsbootadd0r{};
  std::uint32_t nsbootadd1r{};
  std::uint32_t secbootadd0r{};
  std::uint32_t secwm1r1{};
  std::uint32_t secwm1r2{};
  std::uint32_t secwm2r1{};
  std::uint32_t secwm2r2{};
  address       boot{};
};

constexpr std::array<std::string_view, 8> option_registers{
    "FLASH_OPTR",     "FLASH_NSBOOTADD0R", "FLASH_NSBOOTADD1R", "FLASH_SECBOOTADD0R",
    "FLASH_SECWM1R1", "FLASH_SECWM1R2",    "FLASH_SECWM2R1",    "FLASH_SECWM2R2",
};

constexpr std::array<std::string_view, 4> wrp_registers{
    "FLASH_WRP1AR",
    "FLASH_WRP1BR",
    "FLASH_WRP2AR",
    "FLASH_WRP2BR",
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

/**
 * @brief FLASH interface of the STM32U5, RM0456 section 7: the Non-secure register set, and
 *        under TrustZone the Secure register set, page security, HDP and page privilege.
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
 */
class flash final : public dispatch_model<flash, flash_state>,
                    public clock_listener,
                    public voltage_range_listener,
                    public secure_page_controller,
                    public trustzone_aware
{
public:
  flash(model_context& ctx, peripheral_desc const& desc, flash_options const& options);

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
  void               ecc_written(std::uint32_t old, std::uint32_t now);
  void               option_written(std::uint32_t old, std::uint32_t now);
  void               wrp_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto option_locked() const -> bool;
  void               secure_block_written(std::uint32_t old, std::uint32_t now);
  void               hide_written(std::uint32_t old, std::uint32_t now);
  void               privilege_written(std::uint32_t old, std::uint32_t now);
  void               privileged_block_written(std::uint32_t old, std::uint32_t now);

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

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
          -> std::expected<option_words, option_refusal>;

private:
  struct register_set
  {
    reg_ref               key;
    reg_ref               control;
    reg_ref               status;
    field_table<set_role> fields;
    std::uint32_t         clearable{};
  };

  [[nodiscard]] auto make_set(std::string_view prefix) const -> register_set;
  [[nodiscard]] auto field_of(bool secure, set_role role) const -> field_ref;
  [[nodiscard]] auto flag(bool secure, set_role role) const -> bool;
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto errors_pending(bool secure) const -> bool;
  void               fail(bool secure, set_role role);
  void               set_busy(bool on);
  void               set_waiting(bool on);
  void               interrupt();
  void               finish();
  void               store(held_store const& one, std::uint8_t attrs);
  void               hold(held_store const& one, std::uint8_t attrs);
  void start(operation which, std::uint32_t target, std::uint32_t length, bool secure);
  void start_erase(bool secure, std::uint32_t control);
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
  [[nodiscard]] auto wrp_top() const -> std::uint32_t;
  [[nodiscard]] auto wrp_normal(std::uint32_t word) const -> std::uint32_t;
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

  flash_options                                                     options_;
  register_file&                                                    regs_;
  region_desc const*                                                storage_{};
  std::array<register_set, 2>                                       sets_;
  field_ref                                                         ecc_detect_;
  field_ref                                                         ecc_correct_;
  field_ref                                                         ecc_bank_;
  field_ref                                                         ecc_address_;
  irq_line&                                                         line_;
  irq_line*                                                         secure_line_{};
  field_ref                                                         latency_;
  field_ref                                                         low_power_read_;
  core::clock_domain                                                hclk_;
  std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> latency_logged_;
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
  bool                                                              swapped_{};
  std::vector<std::uint32_t>                                        secure_words_;
  std::vector<std::uint32_t>                                        secure_blocks_;
  std::vector<std::uint32_t>                                        privileged_blocks_;
  std::uint32_t                                                     hide_word_{};
  field_ref                                                         invert_;
  std::optional<std::uint32_t>                                      privilege_word_;
  field_ref                                                         nonsecure_privileged_;
  field_ref                                                         secure_privileged_;
  secure_flash_state                                                secure_;
  illegal_access_controller*                                        events_{};
  bool                                                              events_linked_{};
};

auto flash_table(peripheral_desc const& desc, flash_options const& options, bool trustzone)
    -> std::vector<write_entry<flash>>
{
  std::vector<write_entry<flash>> table{
      {"FLASH_ACR",      &flash::access_written        },
      {"FLASH_NSKEYR",   &flash::key_written           },
      {"FLASH_OPTKEYR",  &flash::option_key_written    },
      {"FLASH_NSCR",     &flash::control_written       },
      {"FLASH_NSSR",     &flash::status_written        },
      {"FLASH_ECCR",     &flash::ecc_written           },
      {"FLASH_SECKEYR",  &flash::secure_key_written    },
      {"FLASH_SECCR",    &flash::secure_control_written},
      {"FLASH_SECSR",    &flash::secure_status_written },
      {"FLASH_SECHDPCR", &flash::hide_written          },
  };
  for(auto const name : secure_block_registers)
  {
    if(find_register(desc, name) != nullptr)
    {
      table.push_back({name, &flash::secure_block_written});
    }
  }
  if(trustzone)
  {
    table.push_back({"FLASH_PRIVCFGR", &flash::privilege_written});
    for(auto const name : privileged_block_registers)
    {
      if(find_register(desc, name) != nullptr)
      {
        table.push_back({name, &flash::privileged_block_written});
      }
    }
  }
  for(auto const name : option_registers)
  {
    if(options.option_bytes or find_register(desc, name) != nullptr)
    {
      table.push_back({name, &flash::option_written});
    }
  }
  if(std::ranges::all_of(
         wrp_registers, [&desc](auto name) { return find_register(desc, name) != nullptr; }))
  {
    for(auto const name : wrp_registers)
    {
      table.push_back({name, &flash::wrp_written});
    }
  }
  return table;
}

flash::flash(model_context& ctx, peripheral_desc const& desc, flash_options const& options)
    : dispatch_model{ctx, desc, "FLASH", flash_table(desc, options, ctx.trustzone)},
      options_{options}, regs_{ctx.regs}, sets_{make_set("NS"), make_set("SEC")},
      ecc_detect_{field("FLASH_ECCR", "ECCD")}, ecc_correct_{field("FLASH_ECCR", "ECCC")},
      ecc_bank_{field("FLASH_ECCR", "BK_ECC")}, ecc_address_{field("FLASH_ECCR", "ADDR_ECC")},
      line_{irq("FLASH_IRQn")}, latency_{field("FLASH_ACR", "LATENCY")},
      low_power_read_{field("FLASH_ACR", "LPM")}, hclk_{ctx.clock.domain("hclk")},
      hide_word_{reg("FLASH_SECHDPCR").offset}, invert_{field("FLASH_SECCR", "INV")}
{
  if(ctx.trustzone)
  {
    secure_line_          = &irq("FLASH_S_IRQn");
    privilege_word_       = reg("FLASH_PRIVCFGR").offset;
    nonsecure_privileged_ = field("FLASH_PRIVCFGR", "NSPRIV");
    secure_privileged_    = field("FLASH_PRIVCFGR", "SPRIV");
  }
  for(auto const name : {"FLASH_SECKEYR", "FLASH_SECSR", "FLASH_SECCR", "FLASH_SECHDPCR"})
  {
    secure_words_.push_back(reg(name).offset);
  }
  for(auto const name : option_registers)
  {
    if(has_register(name))
    {
      option_offsets_.push_back(reg(name).offset);
    }
  }
  if(std::ranges::all_of(wrp_registers, [this](auto name) { return has_register(name); }))
  {
    for(auto const name : wrp_registers)
    {
      wrp_offsets_.push_back(reg(name).offset);
      option_offsets_.push_back(reg(name).offset);
    }
    state_.wrp.assign(wrp_offsets_.size(), ~(wrp_top() << 16));
    state_.wrp_active = state_.wrp;
  }
  for(auto const name : secure_block_registers)
  {
    secure_blocks_.push_back(has_register(name) ? reg(name).offset : ~0u);
  }
  for(auto const name : privileged_block_registers)
  {
    privileged_blocks_.push_back(has_register(name) ? reg(name).offset : ~0u);
  }
  auto const own = std::ranges::find_if(ctx.part.regions, [&desc](region_desc const& region) {
    return region.controller == desc.name and not region.physical;
  });
  if(own != ctx.part.regions.end())
  {
    storage_ = &*own;
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
  auto const words = encode_options(ctx.part.part, storage_->size, *options_.option_bytes);
  if(not words)
  {
    throw config_error(desc.name, 0, words.error().key, words.error().expected);
  }
  for(auto const [name, value] : std::views::zip(
          option_registers,
          std::array{
              words->optr, words->nsbootadd0r, words->nsbootadd1r, words->secbootadd0r,
              words->secwm1r1, words->secwm1r2, words->secwm2r1, words->secwm2r2}))
  {
    option_words_.emplace_back(reg(name).offset, value);
  }
  for(auto const name :
      {"FLASH_SECBOOTADD0R", "FLASH_SECWM1R1", "FLASH_SECWM1R2", "FLASH_SECWM2R1",
       "FLASH_SECWM2R2"})
  {
    secure_words_.push_back(reg(name).offset);
  }
  swapped_ = options_.option_bytes->swap_bank;
}

auto flash::make_set(std::string_view prefix) const -> register_set
{
  auto const named = [prefix](std::string_view reg) {
    return std::format("FLASH_{}{}", prefix, reg);
  };
  field_table<set_role> fields{
      u5_set_fields,
      [&](std::string_view reg, std::string_view name) { return field(named(reg), name); },
      [&](std::string_view reg, std::string_view name) {
        auto const* const found = find_register(desc_, named(reg));
        return found != nullptr and std::ranges::contains(found->fields, name, &field_desc::name);
      }};
  std::uint32_t clearable = 0;
  for(auto const role :
      {set_role::end, set_role::operation_error, set_role::program_error,
       set_role::protection_error, set_role::alignment_error, set_role::size_error,
       set_role::sequence_error, set_role::option_error})
  {
    if(auto const found = fields.find(role))
    {
      clearable |= found->mask();
    }
  }
  return {
      .key       = reg(named("KEYR")),
      .control   = reg(named("CR")),
      .status    = reg(named("SR")),
      .fields    = fields,
      .clearable = clearable};
}

auto flash::encode_options(
    std::string_view    part,
    std::uint32_t       flash_size,
    option_bytes const& wanted) -> std::expected<option_words, option_refusal>
{
  auto const refuse = [](std::string_view field, std::string expected) {
    return std::unexpected{
        option_refusal{
                       .key = std::format("option_bytes.{}", field), .expected = std::move(expected)}
    };
  };
  auto const pages  = flash_size / 2 / page_size;
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
  return option_words{
      .optr         = optr,
      .nsbootadd0r  = *nsboot | nsboot_reserved,
      .nsbootadd1r  = nsbootadd1_production,
      .secbootadd0r = secure_word(*secboot | secboot_reserved),
      .secwm1r1     = secure_word(first_word(*secure1)),
      .secwm1r2     = secure_word(*hidden1),
      .secwm2r1     = secure_word(first_word(*secure2)),
      .secwm2r2     = secure_word(*hidden2),
      .boot         = wanted.tzen ? *secboot : *nsboot,
  };
}

void flash::reset(register_file& regs, reset_kind kind)
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
  line_.lower();
  if(secure_line_ != nullptr)
  {
    secure_line_->lower();
    ctx_.services.memory_policy_changed();
  }
  ctx_.nmi.lower();
}

auto flash::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
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
  return dispatch_model::on_read(regs, offset, size, ctx);
}

void flash::option_written(std::uint32_t old, std::uint32_t now)
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

void flash::wrp_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(written_word_, option_locked() ? old : wrp_normal(now));
}

auto flash::wrp_top() const -> std::uint32_t
{
  return (1u << watermark_bits(ctx_.part.part)) - 1;
}

auto flash::wrp_normal(std::uint32_t word) const -> std::uint32_t
{
  auto const fields = wrp_unlock | wrp_top() << 16 | wrp_top();
  return (word & fields) | ~fields;
}

auto flash::wrp_programmed(std::size_t index) const -> std::uint32_t
{
  return state_.wrp[index];
}

auto flash::option_loaded(std::uint32_t offset) const -> std::uint32_t
{
  auto const found =
      std::ranges::find(option_words_, offset, &std::pair<std::uint32_t, std::uint32_t>::first);
  if(found != option_words_.end())
  {
    return found->second;
  }
  for(auto const name : option_registers)
  {
    if(has_register(name) and reg(name).offset == offset)
    {
      auto const& described = describe(name);
      return described.reset & described.reset_mask;
    }
  }
  return 0;
}

auto flash::option_mask(std::uint32_t offset) const -> std::uint32_t
{
  auto const top = (1u << watermark_bits(ctx_.part.part)) - 1;
  for(auto const name : option_registers)
  {
    if(not has_register(name) or reg(name).offset != offset)
    {
      continue;
    }
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
  }
  return ~0u;
}

void flash::warn_dropped_options()
{
  if(option_changed_.empty() or std::exchange(option_logged_, true))
  {
    return;
  }
  auto const offset = option_changed_.front();
  for(auto const name : option_registers)
  {
    if(has_register(name) and reg(name).offset == offset)
    {
      warn(
          name, "written, but OPTSTRT programs only the FLASH_WRPxyR registers: "
                "the machine's option bytes set the others");
      return;
    }
  }
}

auto flash::wrp_protected(std::uint32_t from, std::uint32_t length) const -> bool
{
  if(storage_ == nullptr or wrp_offsets_.empty())
  {
    return false;
  }
  auto const [base, size] = banks();
  auto const top          = wrp_top();
  for(auto at = from; at - from < length; at += page_size)
  {
    if(at - base >= 2 * size)
    {
      continue;
    }
    auto const bank = bank_of(at);
    auto const page = (at - bank_base(at)) / page_size;
    for(std::size_t area = 0; area < 2; ++area)
    {
      auto const word = state_.wrp_active[bank * 2 + area];
      if((word & top) <= page and page <= (word >> 16 & top))
      {
        return true;
      }
    }
  }
  return false;
}

auto flash::loads_options(reset_kind kind) -> bool
{
  return kind == reset_kind::power_on
      or kind == reset_kind::brown_out
      or kind == reset_kind::option_load;
}

void flash::poke_wrp()
{
  for(auto const [index, offset] : std::views::enumerate(wrp_offsets_))
  {
    regs_.poke(offset, state_.wrp_active[static_cast<std::size_t>(index)]);
  }
}

void flash::load_options()
{
  for(auto const [index, offset] : std::views::enumerate(wrp_offsets_))
  {
    regs_.poke(offset, wrp_programmed(static_cast<std::size_t>(index)));
  }
}

void flash::program_options()
{
  if(flag(false, set_role::sequence_error) or flag(false, set_role::option_error))
  {
    load_options();
    return;
  }
  std::vector<std::uint32_t> requested;
  auto                       violation = false;
  for(auto const [index, offset] : std::views::enumerate(wrp_offsets_))
  {
    auto const programmed = wrp_programmed(static_cast<std::size_t>(index));
    auto const word       = wrp_normal(regs_.peek(offset));
    requested.push_back(word);
    violation = violation or (word != programmed and (programmed & wrp_unlock) == 0);
  }
  if(violation)
  {
    load_options();
    fail(false, set_role::option_error);
    return;
  }
  state_.wrp = std::move(requested);
}

auto flash::option_locked() const -> bool
{
  return flag(false, set_role::option_lock);
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
  sync_watches();
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

void flash::sync_watches()
{
  std::vector<std::uint32_t> gone;
  std::vector<std::uint32_t> added;
  std::ranges::set_difference(watched_, state_.torn, std::back_inserter(gone));
  std::ranges::set_difference(state_.torn, watched_, std::back_inserter(added));
  for(auto const line : gone)
  {
    ctx_.services.watch_reads(line, quad_word, false);
  }
  for(auto const line : added)
  {
    ctx_.services.watch_reads(line, quad_word, true);
  }
  watched_ = state_.torn;
}

auto flash::banks() const -> bank_geometry
{
  contract_assert(storage_ != nullptr);
  return {.base = static_cast<std::uint32_t>(storage_->start), .size = storage_->size / 2};
}

auto flash::bank_of(std::uint32_t at) const -> std::uint32_t
{
  auto const [base, size] = banks();
  return ((at - base) / size) ^ (swapped_ ? 1u : 0u);
}

auto flash::bank_base(std::uint32_t at) const -> std::uint32_t
{
  auto const [base, size] = banks();
  return base + (at - base) / size * size;
}

auto flash::page_secure(address at) const -> std::optional<bool>
{
  return page_rule(at).transform([](page_policy const& page) { return page.secure; });
}

auto flash::page_rule(address at) const -> std::optional<page_policy>
{
  if(not ctx_.trustzone or storage_ == nullptr or option_words_.size() != option_registers.size())
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

auto flash::rule_of(std::uint32_t at) const -> page_policy
{
  constexpr std::size_t secwm1r1 = 4;
  constexpr std::size_t secwm2r1 = 6;
  page_policy           policy{
      .secure = block_bit(secure_blocks_, at), .privileged = block_bit(privileged_blocks_, at)};
  if(option_words_.size() != option_registers.size())
  {
    return policy;
  }
  auto const bank        = bank_of(at);
  auto const page        = (at - bank_base(at)) / page_size;
  auto const first       = option_words_[bank == 0 ? secwm1r1 : secwm2r1].second;
  auto const second      = option_words_[(bank == 0 ? secwm1r1 : secwm2r1) + 1].second;
  auto const top         = (1u << watermark_bits(ctx_.part.part)) - 1;
  auto const start       = first & top;
  auto const watermarked = start <= page and page <= (first >> 16 & top);
  auto const hidden      = (second & hdp_enable) != 0
                  and (regs_.peek(hide_word_) >> bank & 1u) != 0
                  and start <= page
                  and page <= (second >> 16 & top);
  policy.secure = policy.secure or watermarked;
  policy.hidden = hidden;
  return policy;
}

auto flash::block_bit(std::span<std::uint32_t const> words, std::uint32_t at) const -> bool
{
  auto const page  = (at - bank_base(at)) / page_size;
  auto const index = bank_of(at) * block_registers_per_bank + page / pages_per_block_register;
  if(index >= words.size() or words[index] == ~0u)
  {
    return false;
  }
  return (regs_.peek(words[index]) >> (page % pages_per_block_register) & 1u) != 0;
}

auto flash::secure_page_size() const -> std::uint32_t
{
  return page_size;
}

auto flash::secure_function() const -> bool
{
  if(not ctx_.trustzone or storage_ == nullptr)
  {
    return false;
  }
  auto const [base, size] = banks();
  auto any                = false;
  for(std::uint32_t at = base; not any and at - base < 2 * size; at += page_size)
  {
    any = rule_of(at).secure;
  }
  return any != (get(regs_, invert_) != 0);
}

auto flash::secure_word() const -> std::uint32_t
{
  return secure_function() ? 1u : 0u;
}

auto flash::bank_start(std::uint32_t bank) const -> std::uint32_t
{
  auto const [base, size] = banks();
  return base + (bank ^ (swapped_ ? 1u : 0u)) * size;
}

void flash::on_memory_read(region_desc const& region, address addr, std::uint8_t size, access_ctx)
{
  auto const at    = physical(region, addr);
  auto const first = at & ~(quad_word - 1);
  auto const last  = (at + size - 1) & ~(quad_word - 1);
  for(auto line = first; line <= last; line += quad_word)
  {
    if(std::ranges::binary_search(state_.torn, line))
    {
      report_double_error(line);
    }
  }
}

void flash::report_double_error(std::uint32_t line)
{
  if(get(regs_, ecc_detect_) != 0 or get(regs_, ecc_correct_) != 0)
  {
    return;
  }
  update(regs_, ecc_address_, line - bank_base(line));
  update(regs_, ecc_bank_, bank_of(line));
  update(regs_, ecc_detect_, 1);
  ctx_.nmi.raise();
}

auto flash::busy() const -> bool
{
  return static_cast<operation>(state_.running) != operation::none;
}

auto flash::field_of(bool secure, set_role role) const -> field_ref
{
  return sets_[secure ? 1 : 0].fields[role];
}

auto flash::flag(bool secure, set_role role) const -> bool
{
  auto const found = sets_[secure ? 1 : 0].fields.find(role);
  return found and get(regs_, *found) != 0;
}

void flash::set_busy(bool on)
{
  update(regs_, field_of(false, set_role::busy), on ? 1 : 0);
  if(ctx_.trustzone)
  {
    update(regs_, field_of(true, set_role::busy), on ? 1 : 0);
  }
}

void flash::set_waiting(bool on)
{
  update(regs_, field_of(false, set_role::waiting), on ? 1 : 0);
  if(ctx_.trustzone)
  {
    update(regs_, field_of(true, set_role::waiting), on ? 1 : 0);
  }
}

auto flash::errors_pending(bool secure) const -> bool
{
  return std::ranges::any_of(error_roles, [&](set_role role) { return flag(secure, role); });
}

void flash::interrupt()
{
  auto const raised = [this](bool secure) {
    return (flag(secure, set_role::end) and flag(secure, set_role::end_irq))
        or (flag(secure, set_role::operation_error) and flag(secure, set_role::error_irq));
  };
  drive(line_, raised(false));
  if(secure_line_ != nullptr)
  {
    drive(*secure_line_, raised(true));
  }
}

void flash::fail(bool secure, set_role role)
{
  update(regs_, field_of(secure, role), 1);
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

void flash::illegal_access(std::string_view enable_field)
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

auto flash::register_refused(std::uint32_t word, access_ctx ctx, bool write) -> bool
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
    return not ctx.privileged and get(regs_, secure_privileged_) != 0;
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
  return not ctx.privileged and get(regs_, nonsecure_privileged_) != 0;
}

auto flash::writable(std::uint32_t from, std::uint32_t length, bool secure, bool privileged) -> bool
{
  auto allowed = true;
  auto event   = false;
  for(auto at = from; at - from < length; at += page_size)
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
  written_      = merge_write(0, offset, size, value);
  written_word_ = offset & ~3u;
  access_       = ctx;
  if(register_refused(written_word_, ctx, true))
  {
    return {};
  }
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
  auto const       secure = ctx_.trustzone and ctx.secure;
  held_store const one{
      .addr    = physical(region, addr),
      .size    = size,
      .value   = value,
      .control = regs_.peek(sets_[secure ? 1 : 0].control.offset),
      .dma     = ctx.by == initiator::dma};
  auto const attrs = static_cast<std::uint8_t>(
      (secure ? held_secure : 0u) | (ctx.privileged ? held_privileged : 0u));
  if(busy())
  {
    hold(one, attrs);
    return {};
  }
  store(one, attrs);
  return {};
}

void flash::hold(held_store const& one, std::uint8_t attrs)
{
  state_.held.push_back(one);
  if(ctx_.trustzone)
  {
    secure_.held_attrs.push_back(attrs);
  }
}

void flash::store(held_store const& one, std::uint8_t attrs)
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
    fail(secure, set_role::sequence_error);
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
    if(addr % (state_.burst ? burst_size : quad_word) != 0)
    {
      fail(secure, set_role::alignment_error);
      return;
    }
    state_.buffer_addr    = addr;
    secure_.buffer_secure = secure;
    set_waiting(true);
  }
  else if(
      state_.burst ? addr != next
                   : addr < state_.buffer_addr or addr >= state_.buffer_addr + quad_word)
  {
    fail(secure, set_role::alignment_error);
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
  set_waiting(false);
  start(operation::program, state_.buffer_addr, length, secure);
}

void flash::start(operation which, std::uint32_t target, std::uint32_t length, bool secure)
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

void flash::start_erase(bool secure, std::uint32_t control)
{
  auto const register_name = secure ? "FLASH_SECCR" : "FLASH_NSCR";
  if(storage_ == nullptr)
  {
    warn(register_name, "erase requested, but no flash region names FLASH as its controller");
    return;
  }
  auto const [base, bank_size] = banks();
  auto const field             = [&](set_role role) { return field_of(secure, role).of(control); };
  auto       target            = base;
  auto       length            = storage_->size;
  auto       which             = operation::mass_erase;
  if(field(set_role::page_erase) != 0)
  {
    auto const pages = bank_size / page_size;
    auto const page  = field(set_role::page);
    if(page >= pages)
    {
      update(regs_, field_of(secure, set_role::start), 0);
      if(not std::exchange(page_logged_, true))
      {
        warn(
            register_name,
            std::format("PNB {} is past the {} pages of a bank: nothing is erased", page, pages));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, register_name, reg(register_name).offset);
      }
      return;
    }
    target = bank_start(field(set_role::bank)) + page * page_size;
    length = page_size;
    which  = operation::page_erase;
  }
  else if(field(set_role::bank1_erase) == 0 or field(set_role::bank2_erase) == 0)
  {
    target = bank_start(field(set_role::bank1_erase) != 0 ? 0 : 1);
    length = bank_size;
    which  = operation::bank_erase;
  }
  if((ctx_.trustzone and not writable(target, length, secure, access_.privileged))
     or wrp_protected(target, length))
  {
    fail(secure, set_role::protection_error);
    update(regs_, field_of(secure, set_role::start), 0);
    return;
  }
  start(which, target, length, secure);
}

void flash::program(bool secure)
{
  std::array<std::byte, burst_size> target_bytes{};
  auto const                        target     = std::span{target_bytes}.first(state_.length);
  auto const                        data_bytes = buffered();
  auto const                        data       = std::span{data_bytes}.first(state_.length);
  state_.filled                                = 0;
  if(not ctx_.services.read_block(state_.target, target, initiator::debugger)
     or refused(state_.target, target, data))
  {
    fail(secure, set_role::program_error);
    return;
  }
  static_cast<void>(ctx_.services.write_block(state_.target, data, initiator::debugger));
}

void flash::finish()
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
  if(errors_pending(secure) == errors and flag(secure, set_role::end_irq))
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

auto flash::hsi16_held() const -> bool
{
  return busy() or std::ranges::any_of(std::array{false, true}, [this](bool secure) {
           return (secure ? ctx_.trustzone : true)
              and (flag(secure, set_role::program) or flag(secure, set_role::start));
         });
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
      range = source->active_level() + 1;
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

void flash::save(std::vector<std::byte>& out) const
{
  if(ctx_.trustzone)
  {
    reflex::serde::bson::serializer{out}.dump(
        trustzone_flash_state{.flash = state_, .secure = secure_});
    return;
  }
  dispatch_model::save(out);
}

void flash::load(std::span<std::byte const> in)
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
    dispatch_model::load(in);
  }
  if(not wrp_offsets_.empty() and state_.wrp.size() != wrp_offsets_.size())
  {
    state_.wrp.assign(wrp_offsets_.size(), ~(wrp_top() << 16));
    load_options();
  }
  if(state_.wrp_active.size() != state_.wrp.size())
  {
    state_.wrp_active = state_.wrp;
  }
  option_changed_.clear();
  latency_logged_.clear();
  sync_watches();
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
  key_written(false, now);
}

void flash::secure_key_written(std::uint32_t, std::uint32_t now)
{
  key_written(true, now);
}

void flash::key_written(bool secure, std::uint32_t now)
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

void flash::option_key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(reg("FLASH_OPTKEYR").offset, 0);
  auto const armed =
      option_locked() and not flag(false, set_role::lock) and not state_.option_locked_up;
  if(armed and not state_.option_first_key and now == option_key1)
  {
    state_.option_first_key = true;
    return;
  }
  if(armed and state_.option_first_key and now == option_key2)
  {
    state_.option_first_key = false;
    update(regs_, field_of(false, set_role::option_lock), 0);
    return;
  }
  state_.option_first_key = false;
  state_.option_locked_up = true;
  rejected_               = true;
}

void flash::control_written(std::uint32_t old, std::uint32_t now)
{
  control_written(false, old, now);
}

void flash::secure_control_written(std::uint32_t old, std::uint32_t now)
{
  control_written(true, old, now);
}

void flash::control_written(bool secure, std::uint32_t old, std::uint32_t now)
{
  auto const of = [&](set_role role, std::uint32_t word) {
    return field_of(secure, role).of(word) != 0;
  };
  auto const control = sets_[secure ? 1 : 0].control.offset;
  if(of(set_role::lock, old))
  {
    regs_.poke(control, old);
    return;
  }
  auto const option_lock       = sets_[secure ? 1 : 0].fields.find(set_role::option_lock);
  auto const option_field      = sets_[secure ? 1 : 0].fields.find(set_role::option_start);
  auto const option_load_field = sets_[secure ? 1 : 0].fields.find(set_role::option_load);
  auto const erase_select      = [&](std::uint32_t word) {
    return of(set_role::page_erase, word)
        or of(set_role::bank1_erase, word)
        or of(set_role::bank2_erase, word);
  };
  auto const other_held =
      ctx_.trustzone and (flag(not secure, set_role::program) or flag(not secure, set_role::start));
  auto const hsi16_before = of(set_role::program, old) or of(set_role::start, old) or other_held;
  auto const program_set  = of(set_role::program, now) and not of(set_role::program, old);
  auto const start_set    = of(set_role::start, now) and not of(set_role::start, old);
  auto const unlocked     = option_lock and option_lock->of(old) == 0;
  auto const option_start = unlocked and option_field and option_field->of(now) != 0;
  auto const waiting      = flag(false, set_role::waiting);
  auto       sequence     = false;
  if(of(set_role::program, now) and erase_select(now) and (program_set or not erase_select(old)))
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
  if(option_field)
  {
    now = option_field->with(now, 0);
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
  if(start_set and not busy())
  {
    if(errors_pending(secure)
       or not erase_select(now)
       or (of(set_role::page_erase, now)
           and (of(set_role::bank1_erase, now) or of(set_role::bank2_erase, now))))
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
  auto const hsi16_after = hsi16_held();
  if(hsi16_after != hsi16_before)
  {
    hold_hsi16(hsi16_after);
  }
  interrupt();
}

void flash::status_written(std::uint32_t old, std::uint32_t)
{
  status_written(false, old);
}

void flash::secure_status_written(std::uint32_t old, std::uint32_t)
{
  status_written(true, old);
}

void flash::status_written(bool secure, std::uint32_t old)
{
  auto const& set = sets_[secure ? 1 : 0];
  regs_.poke(set.status.offset, old & ~(written_ & set.clearable));
  interrupt();
}

void flash::secure_block_written(std::uint32_t old, std::uint32_t now)
{
  auto const index   = std::ranges::find(secure_blocks_, written_word_) - secure_blocks_.begin();
  auto const partner = privileged_blocks_[static_cast<std::size_t>(index)];
  auto const allowed = access_.privileged or partner == ~0u ? ~0u : ~regs_.peek(partner);
  auto const kept    = (now & allowed) | (old & ~allowed);
  regs_.poke(written_word_, kept);
  policy_written(old, kept);
}

void flash::hide_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(written_word_, old | now);
  policy_written(old, old | now);
}

void flash::privilege_written(std::uint32_t old, std::uint32_t now)
{
  auto allowed = 0u;
  if(access_.privileged)
  {
    allowed |= nonsecure_privileged_.mask();
    if(access_.secure)
    {
      allowed |= secure_privileged_.mask();
    }
  }
  regs_.poke(written_word_, (now & allowed) | (old & ~allowed));
}

void flash::privileged_block_written(std::uint32_t old, std::uint32_t now)
{
  auto allowed = ~0u;
  if(not access_.secure)
  {
    auto const index =
        std::ranges::find(privileged_blocks_, written_word_) - privileged_blocks_.begin();
    auto const bank = static_cast<std::uint32_t>(index) / block_registers_per_bank;
    auto const first =
        static_cast<std::uint32_t>(index) % block_registers_per_bank * pages_per_block_register;
    for(std::uint32_t bit = 0; bit < pages_per_block_register; ++bit)
    {
      auto const at = bank_start(bank) + (first + bit) * page_size;
      if(first + bit >= banks().size / page_size or rule_of(at).secure)
      {
        allowed &= ~(1u << bit);
      }
    }
  }
  auto const kept = (now & allowed) | (old & ~allowed);
  regs_.poke(written_word_, kept);
  policy_written(old, kept);
}

void flash::policy_written(std::uint32_t old, std::uint32_t now)
{
  if(old != now)
  {
    ctx_.services.memory_policy_changed();
    ctx_.services.end_after_instruction();
  }
}

void flash::ecc_written(std::uint32_t old, std::uint32_t now)
{
  auto const flags = ecc_detect_.mask() | ecc_correct_.mask();
  regs_.poke(ecc_detect_.offset, (now & ~flags) | (old & flags & ~written_));
  if(get(regs_, ecc_detect_) == 0)
  {
    ctx_.nmi.lower();
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
  if(not part.part.starts_with("STM32U5") or storage == part.regions.end())
  {
    return std::unexpected{
        option_refusal{
                       .key      = "option_bytes",
                       .expected = std::format(
                "a part whose built-in FLASH model encodes option bytes, an STM32U5 with a flash "
                "region its FLASH controls, not {}", part.part)}
    };
  }
  return flash::encode_options(part.part, storage->size, wanted)
      .transform([](option_words const& words) { return words.boot; });
}

auto flash_factory(flash_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<flash>(ctx, desc, options);
  };
}
}
