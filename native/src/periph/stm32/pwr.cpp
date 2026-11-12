#include "backup_domain.hpp"
#include "family_names.hpp"
#include "gpio_peers.hpp"
#include "gtzc.hpp"
#include "model_base.hpp"
#include "pwr_peers.hpp"
#include "pwr_rows.hpp"
#include "secure_config.hpp"
#include "voltage_range.hpp"

#include <popcorn/periph/layer_keeper.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
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
constexpr std::uint32_t voltage_ready   = 0;
constexpr std::uint32_t booster_ready   = 1;
constexpr std::uint32_t voltage_step    = 2;
constexpr std::uint32_t regulator_ready = 3;

enum class low_power_mode : std::uint8_t
{
  stop,
  stop0,
  stop1,
  stop2,
  stop3,
  standby,
  shutdown,
};

constexpr std::array<low_power_mode, 2> h5_modes_by_lpms{
    low_power_mode::stop0, low_power_mode::standby};

constexpr std::array<low_power_mode, 2> l1_modes_by_pdds{
    low_power_mode::stop, low_power_mode::standby};

constexpr std::array<low_power_mode, 8> l4_modes_by_lpms{
    low_power_mode::stop0,    low_power_mode::stop1,    low_power_mode::stop2,
    low_power_mode::standby,  low_power_mode::shutdown, low_power_mode::shutdown,
    low_power_mode::shutdown, low_power_mode::shutdown};

/** @brief A `vos_levels` entry for a VOS value the manual forbids. */
constexpr std::uint8_t vos_forbidden = 0xFF;

constexpr std::array<low_power_mode, 8> modes_by_lpms{
    low_power_mode::stop0,    low_power_mode::stop1,   low_power_mode::stop2,
    low_power_mode::stop3,    low_power_mode::standby, low_power_mode::standby,
    low_power_mode::shutdown, low_power_mode::shutdown};

enum class pwr_role : std::uint8_t
{
  vos,
  vos_ready,
  active_vos,
  active_vos_ready,
  mode,
  booster,
  booster_ready,
  regulator_select,
  regulator_status,
  io_supply_valid,
  usb_supply_valid,
};

enum class guard_source : std::uint8_t
{
  own,
  system_clock,
  port_pin,
  peripheral,
};

struct pwr_rule
{
  std::uint32_t              offset{};
  std::uint32_t              bits{};
  bool                       raz = true;
  guard_source               source{};
  field_ref                  own{};
  std::string                peer{};
  std::uint8_t               pin{};
  std::optional<std::size_t> unit{};
};

struct pwr_function
{
  field_ref        bits;
  std::string_view reg;
  std::string_view name;
  bool             logged{};
};

struct pwr_state
{
  std::array<std::uint32_t, 2> generation{};
  std::uint32_t                settled_level{};
  std::uint32_t                regulator_generation{};

  friend auto operator==(pwr_state const&, pwr_state const&) -> bool = default;
};

class pwr;

/** @brief Bits a `PWR_SECCFGR` bit secures. */
struct pwr_secured
{
  /** @brief Field of `PWR_SECCFGR`. */
  std::string_view bit;
  /** @brief Register it secures. */
  std::string_view reg;
  /** @brief Field it secures, empty for the whole register. */
  std::string_view field = {};
  /** @brief A Non-secure read of the bits reads 0, otherwise it reads them. */
  bool raz = true;
};

/** @brief A field of every wakeup pin x that `PWR_SECCFGR.WUPxSEC` secures, `<prefix>x`. */
struct pwr_wakeup_field
{
  /** @brief Register of the field. */
  std::string_view reg;
  /** @brief Field name before the pin number. */
  std::string_view prefix;
  /** @brief A Non-secure read of the bits reads 0, otherwise it reads them. */
  bool raz = true;
};

/** @brief ACTVOS stepping of a VOS change, measured on silicon. */
struct vos_stepping
{
  /** @brief Time between two one-level ACTVOS steps. */
  std::chrono::nanoseconds step;
  /** @brief Time from the last ACTVOS step to VOSRDY and ACTVOSRDY. */
  std::chrono::nanoseconds settle;
};

/** @brief Time from a REGSEL change to REGS, per ACTVOS code, 0 range 4 to 3 range 1. */
struct regulator_switch
{
  /** @brief LDO to SMPS. */
  std::array<std::chrono::nanoseconds, 4> to_smps;
  /** @brief SMPS to LDO. */
  std::array<std::chrono::nanoseconds, 4> to_ldo;
};

/** @brief What differs between the PWR blocks one model serves. */
struct pwr_variant
{
  /** @brief Register and field of every role, an empty name for a role the family lacks. */
  std::span<named_field<pwr_role> const> fields;
  /** @brief Low-power mode of every value of the mode field. */
  std::span<low_power_mode const> modes;
  /** @brief Performance level, 0 the highest, of every value of the VOS field, vos_forbidden
   *         for a value the manual forbids. A firmware write of a forbidden value keeps VOS,
   *         logs one warning per machine life and raises model_error under
   *         strict_peripherals. */
  std::span<std::uint8_t const> vos_levels;
  /** @brief The VOS ready bit reads 1 when the voltage is ready. */
  bool vos_ready_set = true;
  /** @brief The active VOS ready bit reads 1 when the voltage is ready. */
  bool active_ready_set = true;
  /** @brief ACTVOS moves one level toward VOS at the VOS write and one more every `step`, and
   *         the ready bits set `settle` after the last step. Without it ACTVOS takes the new VOS
   *         when the board's ready delay ends. */
  std::optional<vos_stepping> stepping{};
  /** @brief The package has the SMPS: REGS follows a REGSEL change after the time of the range
   *         ACTVOS gives at the write. Without it a firmware write that sets REGSEL logs one
   *         warning per machine life and raises model_error under strict_peripherals, and REGS
   *         keeps 0, the LDO. */
  std::optional<regulator_switch> smps{};
  /** @brief The TrustZone configuration register, empty for a family without TrustZone. */
  std::string_view secure_register;
  /** @brief Write handlers of every machine. */
  std::span<write_entry<pwr> const> writes;
  /** @brief Write handlers added under TrustZone. */
  std::span<write_entry<pwr> const> trustzone_writes;
  /** @brief Bits each `PWR_SECCFGR` group bit secures. */
  std::span<pwr_secured const> secured;
  /** @brief Fields of each wakeup pin its `WUPxSEC` bit secures. */
  std::span<pwr_wakeup_field const> wakeup_fields;
  /** @brief Voltage scaling fields `RCC_SECCFGR.SYSCLKSEC` secures. An optional one is secured
   * when the description has it, a required one throws config_error when it lacks it. */
  std::span<field_name const> system_clock_fields;
  /** @brief Fields a peripheral reset of the unit, RCC PWRRST, leaves at their value. */
  std::span<field_name const> peripheral_reset_kept;
  /** @brief Fields a pin, software, watchdog, window watchdog, low-power or option-load reset
   *         leaves at their value. A power-on or brown-out reset clears them. */
  std::span<field_name const> system_reset_kept{};
  /** @brief Fields of the backup domain, every reset but a power-on reset leaves at their value. */
  std::span<field_name const> backup_reset_kept{};
  /** @brief Registers a system reset leaves as they are, the write-once lock included, when no
   *         TrustZone makes the model claim them. */
  std::span<std::string_view const> system_reset_words{};
  /** @brief Enables of functions the model does not run. A firmware write that sets one from 0
   *         logs one warning per field per machine life and stores it. Under strict_peripherals
   *         it raises model_error and the register keeps its value before the write. */
  std::span<field_name const> unmodelled{};
};

/**
 * @brief PWR voltage scaling of the STM32U5, RM0456 section 10.10.4, and of the STM32H5, RM0481
 *        10.11.5 and 10.11.6. A VOS write that steps to a higher voltage clears VOSRDY until the
 *        board's ready delay ends, one that steps to a lower voltage keeps VOSRDY, the voltage
 *        being at or above the selected level, RM0456 10.10.4 and RM0481 10.11.6. ACTVOSRDY clears
 *        on every VOS change until the delay ends, and BOOSTRDY follows BOOSTEN the same way on the
 *        U5. ACTVOS takes the new range at the end of the delay and the FLASH model is told for its
 *        latency check. The H5 reads them in PWR_VOSSR and writes VOS in PWR_VOSCR, and steps
 *        ACTVOS one level at the write and one more every 10.4 us, with VOSRDY and ACTVOSRDY set
 *        15.2 us after the last step: VOS3 to VOS0 is ready 36 us after the write, as measured on
 *        an STM32H573. The voltage range FLASH checks against is the one at the last ready, so
 *        the steps change it only when the ready bits set.
 *        Low-power modes are not modelled: the first write to the mode register, PWR_CR1 on the
 *        U5 and PWR_PMCR on the H5, logs one warning per machine life naming the LPMS mode, and
 *        a deep-sleep WFI idles as a plain WFI.
 *
 *        Regulator selection, RM0456 10.5 and 10.10.3: on a U5 package with the SMPS, the part
 *        names ending in Q, PWR_SVMSR.REGS takes the value of PWR_CR3.REGSEL after the DS13737
 *        Table 75 typical transition time of the range ACTVOS gives at the REGSEL write, 14 to
 *        17 us. Every reset clears both, the regulator being the LDO. On a package without the
 *        SMPS REGSEL is reserved: REGS stays 0, and a firmware write that changes REGSEL from 0
 *        to 1 logs one warning per machine life. Under strict_peripherals that write raises
 *        model_error and `PWR_CR3` keeps its value before the write. Otherwise REGSEL holds the
 *        written value.
 *
 *        USB supply, RM0456 10.4.6 and 10.10.5: `PWR_SVMCR.USV` declares VDDUSB valid. The
 *        OTG_FS model reads it through usb_supply_source and is told of every change through
 *        usb_supply_listener. The other `PWR_SVMCR` fields are storage.
 *
 *        TrustZone, RM0456 10.8 and Table 109: the bits a `PWR_SECCFGR` bit secures, the VOSR
 *        bits `RCC_SECCFGR.SYSCLKSEC` secures, the PUCRx and PDCRx bits of the Secure pins of
 *        GPIOx and `PWR_UCPDR` while the TZSC makes UCPD1 Secure are RAZ/WI to a Non-secure
 *        access, with no event. `PWR_PRIVCFGR.SPRIV` makes the Secure bits privileged, `NSPRIV`
 *        the securable bits left Non-secure, RAZ/WI to an unprivileged access. The status bits
 *        Table 109 does not list stay readable by every transaction. `PWR_SECCFGR` reads for
 *        every transaction and takes Secure writes, privileged with SPRIV set. A Non-secure
 *        write to it is ignored with an illegal access event. `PWR_PRIVCFGR` takes privileged
 *        writes, SPRIV Secure ones only. Under TZEN=0 `PWR_SECCFGR` is RAZ/WI, its storage
 *        keeps its reset, and `PWR_PRIVCFGR` is storage.
 *
 *        The H5 follows RM0481 10.9 and Table 114 the same way: LPMSEC secures `PWR_PMCR`,
 *        VUSBSEC `PWR_USBSCR`, VBSEC `PWR_BDCR` and `PWR_DBPCR`, RETSEC `PWR_IORETR`, SCMSEC
 *        `PWR_SCCR` and `PWR_VMCR`, each WI to a Non-secure write and read back by a Non-secure
 *        read. `WUPxSEC` secures `WUPENx`, `WUPPx` and `WUPPUPDx` of `PWR_WUCR` RAZ/WI and
 *        `CWUFx` of `PWR_WUSCR` WI, SYSCLKSEC the `PWR_VOSCR.VOS` field RAZ/WI, and the UCPD1
 *        SEC bit `PWR_UCPDR` RAZ/WI. SCMSEC is the `PWR_SECCFGR` bit the register descriptions
 *        of RM0481 10.11.11 and 10.11.12 name.
 *
 *        The STM32L1 follows RM0038 5.4.1 and 5.4.2: a VOS write to another range sets
 *        PWR_CSR.VOSF until the board's ready delay ends, then the FLASH model is told. VOS 00 is
 *        forbidden: a firmware write of it keeps the range, logs one warning per machine life and
 *        raises model_error under strict_peripherals. The first PDDS change logs the low-power
 *        mode warning. DBP, LPSDSR, LPRUN, ULP, FWU, PVD and the wakeup flags are storage. A
 *        peripheral reset through RCC PWRRST keeps FWU and ULP.
 *
 *        The STM32L47x/L48x follows RM0351 5.1.8 and 5.4: a VOS write to the other range sets
 *        PWR_SR2.VOSF until the board's ready delay ends, then the FLASH model is told. VOSF sets
 *        in both directions, DS10198 Table 53 giving tVOST, 20 us typical and 40 us maximum, from
 *        Range 2 to Range 1 and from Range 1 to Range 2. VOS 00 and 11 cannot be written: a
 *        firmware write of either keeps the range, logs one warning per machine life and raises
 *        model_error under strict_peripherals. The first LPMS change logs the low-power mode
 *        warning, LPMS 000 to 011 Stop 0, Stop 1, Stop 2 and Standby, 1xx Shutdown. A firmware
 *        write that sets LPR, PVDE, a PVMEx, an EWUPx or APC from 0 logs one warning per field
 *        per machine life, and under strict_peripherals raises model_error with the register
 *        kept at its value before the write. IOSV is storage the GPIO model reads through
 *        io_supply_source, USV the OTG_FS model reads through usb_supply_source and is told of
 *        through usb_supply_listener. DBP, PLS, RRS, EIWUL, PWR_CR4, PWR_SCR, PWR_PUCRx and
 *        PWR_PDCRx are storage. REGLPS, REGLPF, PVDO, the PVMOx flags and the wakeup and
 *        Standby flags stay 0.
 *
 * @limits pwr PWR
 *
 * @group u5,h5 unmodelled
 * - The low-power modes `PWR_CR1.LPMS` selects on the U5 and `PWR_PMCR.LPMS` on the H5 are not
 *   modelled. A deep-sleep WFI idles as a plain WFI.
 * - The backup domain monitoring that `MONEN` enables is not modelled, in `PWR_BDCR1` on the U5
 *   and `PWR_BDCR` on the H5: its flags and tamper detection are not modelled. A firmware write
 *   that sets `MONEN` from 0 logs one warning per machine life and stores the bit. Under
 *   `strict_peripherals` it is a `model_error` and the register keeps its value before the write.
 * - `BREN`, the backup RAM retention in Standby and VBAT modes, and `VBE` and `VBRS`, the VBAT
 *   battery charging and its resistor, are stored and have no modelled effect, in `PWR_BDCR1` and
 *   `PWR_BDCR2` on the U5, RM0456 10.10.9 and 10.10.10, and in `PWR_BDCR` on the H5, RM0481
 *   10.11.7. A write of them logs nothing and raises nothing under `strict_peripherals`. The L4
 *   `PWR_CR4.VBE` and `PWR_CR4.VBRS` are stored, RM0351 5.1.5.
 *
 * @group u5 rule
 * - STM32U5: the voltage scaling ready bits follow a VOS write after the board's `ready_delay`, 50
 *   us by default. A write that steps to a higher voltage clears VOSRDY for that delay, one that
 *   steps to a lower voltage keeps VOSRDY set. ACTVOSRDY clears for the delay on every change.
 *
 * @group h5 rule
 * - STM32H5: a VOS write moves ACTVOS one level toward VOS at once and one more level every 10.4
 *   us, and VOSRDY and ACTVOSRDY set 15.2 us after the last step. VOS3 to VOS0 reads 0x4000, 0x8000
 *   and 0xC000 in PWR_VOSSR, then 0xE008 36 us after the write, as measured on an STM32H573. A step
 *   to a lower voltage keeps VOSRDY set and takes the same steps, and a change of one or two levels
 *   follows the same rule. The measurement covers VOS3 to VOS0 only. FLASH checks its latency
 *   against the range of the last ready.
 * - STM32H5: PWR_USBSCR is storage, the USB model runs whatever USB33DEN and USB33SV hold.
 * - STM32H5: `PWR_SCCR` resets with SMPSEN (0x200) on the packages with the SMPS regulator, the
 *   part names ending in Q, and with LDOEN (0x100) on the others, RM0481 10.11.11. Both bits are
 *   read-only. The supply the model runs is the board's.
 * - STM32H5: a pin, software, watchdog, window watchdog, low-power or option-load reset keeps
 *   `PWR_SCCR.BYPASS`, RM0481 10.11.11, and without TrustZone the write-once lock of the
 *   register stays closed. A power-on or brown-out reset clears BYPASS and opens the lock. With
 *   TrustZone BYPASS stays writable after its first write, a deviation from RM0481 10.11.11, the
 *   model claims the register and a claimed register has no write-once lock.
 * - STM32H5: every reset but a power-on one keeps the fields of `PWR_BDCR`, RM0481 10.11.7. A
 *   `RCC_BDCR.VSWRST` reset clears them.
 *
 * @group u5 rule "STM32U5 PWR_CR3 and supply selection"
 * - STM32U5 packages with the SMPS, the part names ending in Q: `PWR_SVMSR.REGS` takes the value of
 *   `PWR_CR3.REGSEL` after the typical transition time of DS13737 Table 75 for the range ACTVOS
 *   gives at the REGSEL write: LDO to SMPS 14, 17, 16 and 16 us in range 4 to 1, SMPS to LDO 16,
 *   15, 14 and 14 us. The figures are the DS13737 typical values, applied to every U5 SMPS part.
 *   Every reset sets both to 0, the LDO.
 * - STM32U5 packages without the SMPS: REGSEL is reserved, RM0456 10.10.3, and REGS stays 0. A
 *   firmware write that changes REGSEL from 0 to 1 logs one warning per machine life. Under
 *   `strict_peripherals` it is a `model_error` and `PWR_CR3` keeps its value before the write.
 *   Otherwise the firmware REGSEL write is stored, an inferred rule.
 * - STM32U5: `PWR_CR3.FSTEN` is storage. A pin, software, watchdog, window watchdog, low-power or
 *   option-load reset keeps FSTEN and the fields of `PWR_BDCR2`, RM0456 10.10.3 and 10.10.10, a
 *   power-on or brown-out reset clears them. REGSEL clears on every reset.
 * - STM32U5: every reset but a power-on one keeps the fields of `PWR_BDCR1`, RM0456 10.10.9. A
 *   backup domain reset, `RCC_BDCR.BDRST`, clears them, RM0456 11.3.3, and leaves `PWR_CR3` and
 *   `PWR_BDCR2` as they are.
 *
 * @group l1 rule
 * - STM32L1: a VOS write to another range sets `PWR_CSR.VOSF` for the board's `ready_delay`, 50 us
 *   by default, in either direction, and the new range applies when VOSF clears, RM0038 5.4.1 and
 *   5.4.2. The delay is not a silicon figure. A write of VOS 00 keeps the range, RM0038 5.4.1. A
 *   firmware write of it also logs one warning per machine life and is a `model_error` under
 *   `strict_peripherals`.
 * - STM32L1: the low-power modes `PWR_CR.PDDS` selects are not modelled, the first PDDS change logs
 *   one warning. `DBP` does not gate the RTC domain bits of `RCC_CSR` or the RTC registers. LPSDSR,
 *   LPRUN, ULP, FWU, the PVD and the wakeup flags are storage. A peripheral reset through
 *   `RCC_APB1RSTR.PWRRST` keeps FWU and ULP, RM0038 5.4.1.
 *
 * @group l4 rule
 * - STM32L47x/L48x: a VOS write to the other range sets `PWR_SR2.VOSF` for the board's
 *   `ready_delay`, 50 us by default, in either direction, and the new range applies when VOSF
 *   clears, RM0351 5.1.8 and 5.4.6. DS10198 Table 53 gives tVOST 20 us typical and 40 us maximum,
 *   the board option sets the delay. A write of VOS 00 or 11 keeps the range, RM0351 5.4.1. A
 *   firmware write of either also logs one warning per machine life and is a `model_error` under
 *   `strict_peripherals`.
 * - STM32L47x/L48x: the low-power modes `PWR_CR1.LPMS` selects are not modelled, the first LPMS
 *   change logs one warning naming the mode, and a deep-sleep WFI idles as a plain WFI. A firmware
 *   write that sets `LPR`, `PVDE`, `PVME1` to `PVME4`, `EWUP1` to `EWUP5` or `APC` from 0 logs one
 *   warning per field per machine life and stores the bit. Under `strict_peripherals` it is a
 *   `model_error` and the register keeps its value before the write. `REGLPS`, `REGLPF`, `PVDO`,
 *   the `PVMOx` flags and the wakeup and Standby flags stay 0.
 * - STM32L47x/L48x: `RCC_BDCR` and the RTC registers take writes whatever `DBP` holds. `USV`,
 *   `IOSV`, `PLS`, `RRS`, `EIWUL`, `PWR_CR4`, `PWR_SCR` and `PWR_PUCRx` and `PWR_PDCRx` are
 *   storage. A peripheral reset through `RCC_APB1RSTR1.PWRRST` resets every PWR register.
 *
 * @group l4 note
 * - STM32L47x/L48x PWR errata, ES0250 2.2: a `PWR_CR4` write takes effect whatever low-power entry
 *   follows it, 2.2.21. The PWR model raises no reset, so a short run sequence gives no brown-out
 *   reset, 2.2.5.
 */
class pwr final : public dispatch_model<pwr, pwr_state>,
                  public voltage_range_source,
                  public io_supply_source,
                  public usb_supply_source,
                  public backup_domain,
                  public layer_keeper,
                  public trustzone_aware
{
public:
  pwr(model_context&         ctx,
      peripheral_desc const& desc,
      pwr_variant const&     variant,
      pwr_options const&     options);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto active_level() const -> std::uint32_t override;
  [[nodiscard]] auto io_supply_valid() const -> bool override;
  [[nodiscard]] auto usb_supply_valid() const -> bool override;
  auto               on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;

  void               vosr_written(std::uint32_t old, std::uint32_t now);
  void               backup_reset() override;
  [[nodiscard]] auto keeps_over_reset(std::uint32_t offset, reset_kind kind) const -> bool override;
  void               tell_usb_supply(bool before);
  void               cr1_written(std::uint32_t old, std::uint32_t now);
  void               control_written(std::uint32_t old, std::uint32_t now);
  void               cr3_written(std::uint32_t old, std::uint32_t now);
  void               functions_written(std::uint32_t old, std::uint32_t now);

  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void               add_rules();
  [[nodiscard]] auto rule_secure(pwr_rule const& rule) const -> bool;
  void               step_active(vos_stepping const& stepping, std::uint64_t generation);
  [[nodiscard]] auto active_ready() const -> bool;

  pwr_options                options_;
  register_file&             regs_;
  pwr_variant const&         variant_;
  field_table<pwr_role>      names_;
  bool                       low_power_logged_{};
  bool                       reserved_select_logged_{};
  bool                       forbidden_vos_logged_{};
  initiator                  writer_{initiator::cpu};
  std::uint32_t              written_{};
  std::vector<pwr_function>  functions_;
  std::optional<reg_ref>     secure_;
  reg_ref                    privilege_;
  field_ref                  secure_privileged_;
  field_ref                  nonsecure_privileged_;
  std::optional<field_ref>   system_clock_;
  std::vector<pwr_rule>      rules_;
  std::optional<std::size_t> unit_;
  illegal_event              illegal_;
  std::vector<field_ref>     peripheral_kept_;
  std::vector<field_ref>     system_kept_;
  std::vector<field_ref>     backup_kept_;
  std::vector<std::uint32_t> backup_defaults_;
  std::vector<std::uint32_t> system_words_;
};

constexpr std::array<write_entry<pwr>, 8> u5_writes{
    {
     {"PWR_CR1", &pwr::cr1_written},
     {"PWR_CR3", &pwr::cr3_written},
     {"PWR_VOSR", &pwr::vosr_written},
     {"PWR_SVMCR", nullptr},
     {"PWR_SVMSR", nullptr},
     {"PWR_SECCFGR", nullptr},
     {"PWR_BDCR1", &pwr::functions_written},
     {"PWR_BDCR2", &pwr::functions_written},
     }
};

constexpr auto u5_trustzone_writes = std::to_array<write_entry<pwr>>({
    {"PWR_CR2",      nullptr},
    {"PWR_WUCR1",    nullptr},
    {"PWR_WUCR2",    nullptr},
    {"PWR_WUCR3",    nullptr},
    {"PWR_DBPR",     nullptr},
    {"PWR_UCPDR",    nullptr},
    {"PWR_PRIVCFGR", nullptr},
    {"PWR_SR",       nullptr},
    {"PWR_WUSCR",    nullptr},
    {"PWR_APCR",     nullptr},
    {"PWR_PUCRA",    nullptr},
    {"PWR_PDCRA",    nullptr},
    {"PWR_PUCRB",    nullptr},
    {"PWR_PDCRB",    nullptr},
    {"PWR_PUCRC",    nullptr},
    {"PWR_PDCRC",    nullptr},
    {"PWR_PUCRD",    nullptr},
    {"PWR_PDCRD",    nullptr},
    {"PWR_PUCRE",    nullptr},
    {"PWR_PDCRE",    nullptr},
    {"PWR_PUCRF",    nullptr},
    {"PWR_PDCRF",    nullptr},
    {"PWR_PUCRG",    nullptr},
    {"PWR_PDCRG",    nullptr},
    {"PWR_PUCRH",    nullptr},
    {"PWR_PDCRH",    nullptr},
    {"PWR_PUCRI",    nullptr},
    {"PWR_PDCRI",    nullptr},
});

constexpr auto u5_secured = std::to_array<pwr_secured>({
    {.bit = "LPMSEC", .reg = "PWR_CR1"},
    {.bit = "LPMSEC", .reg = "PWR_CR2"},
    {.bit = "VDMSEC", .reg = "PWR_CR3"},
    {.bit = "VDMSEC", .reg = "PWR_SVMCR"},
    {.bit = "VBSEC", .reg = "PWR_BDCR1"},
    {.bit = "VBSEC", .reg = "PWR_BDCR2"},
    {.bit = "VBSEC", .reg = "PWR_DBPR"},
    {.bit = "APCSEC", .reg = "PWR_APCR"},
    {.bit = "LPMSEC", .reg = "PWR_SR", .field = "CSSF"},
});

constexpr auto u5_wakeup_fields = std::to_array<pwr_wakeup_field>({
    {.reg = "PWR_WUCR1", .prefix = "WUPEN"},
    {.reg = "PWR_WUCR2", .prefix = "WUPP" },
    {.reg = "PWR_WUCR3", .prefix = "WUSEL"},
    {.reg = "PWR_WUSCR", .prefix = "CWUF" },
});

constexpr auto u5_system_clock_fields = std::to_array<field_name>({
    {.reg = "PWR_VOSR", .field = "VOS"},
    {.reg = "PWR_VOSR", .field = "BOOSTEN"},
    {.reg = "PWR_VOSR", .field = "USBPWREN", .optional = true},
    {.reg = "PWR_VOSR", .field = "USBBOOSTEN", .optional = true},
});

/** @brief STM32U5 PWR, RM0456 10.10: VOS 00 range 4 to 11 range 1, ready bits read 1. */
constexpr std::array<named_field<pwr_role>, role_count<pwr_role>> u5_names{
    {
     {pwr_role::vos, {"PWR_VOSR", "VOS"}},
     {pwr_role::vos_ready, {"PWR_VOSR", "VOSRDY"}},
     {pwr_role::active_vos, {"PWR_SVMSR", "ACTVOS"}},
     {pwr_role::active_vos_ready, {"PWR_SVMSR", "ACTVOSRDY"}},
     {pwr_role::mode, {"PWR_CR1", "LPMS"}},
     {pwr_role::booster, {"PWR_VOSR", "BOOSTEN"}},
     {pwr_role::booster_ready, {"PWR_VOSR", "BOOSTRDY"}},
     {pwr_role::regulator_select, {"PWR_CR3", "REGSEL"}},
     {pwr_role::regulator_status, {"PWR_SVMSR", "REGS"}},
     {pwr_role::io_supply_valid, {"", ""}},
     {pwr_role::usb_supply_valid, {"PWR_SVMCR", "USV"}},
     }
};

constexpr std::array<std::uint8_t, 4> u5_vos_levels{3, 2, 1, 0};

/** @brief STM32H5 PWR, RM0481 10.11: VOS 00 VOS3 to 11 VOS0, VOSRDY and ACTVOSRDY read 1 when
 *         ready, no EPOD booster, LPMS 0 Stop and 1 Standby. */
constexpr std::array<named_field<pwr_role>, role_count<pwr_role>> h5_names{
    {
     {pwr_role::vos, {"PWR_VOSCR", "VOS"}},
     {pwr_role::vos_ready, {"PWR_VOSSR", "VOSRDY"}},
     {pwr_role::active_vos, {"PWR_VOSSR", "ACTVOS"}},
     {pwr_role::active_vos_ready, {"PWR_VOSSR", "ACTVOSRDY"}},
     {pwr_role::mode, {"PWR_PMCR", "LPMS"}},
     {pwr_role::booster, {"", ""}},
     {pwr_role::booster_ready, {"", ""}},
     {pwr_role::regulator_select, {"", ""}},
     {pwr_role::regulator_status, {"", ""}},
     {pwr_role::io_supply_valid, {"", ""}},
     {pwr_role::usb_supply_valid, {"", ""}},
     }
};

constexpr std::array<std::uint8_t, 4> h5_vos_levels{3, 2, 1, 0};

constexpr auto h5_trustzone_writes = std::to_array<write_entry<pwr>>({
    {"PWR_USBSCR",   nullptr},
    {"PWR_DBPCR",    nullptr},
    {"PWR_IORETR",   nullptr},
    {"PWR_SCCR",     nullptr},
    {"PWR_VMCR",     nullptr},
    {"PWR_WUCR",     nullptr},
    {"PWR_WUSCR",    nullptr},
    {"PWR_UCPDR",    nullptr},
    {"PWR_PRIVCFGR", nullptr},
});

constexpr auto h5_secured = std::to_array<pwr_secured>({
    {.bit = "LPMSEC",  .reg = "PWR_PMCR",   .raz = false},
    {.bit = "VUSBSEC", .reg = "PWR_USBSCR", .raz = false},
    {.bit = "VBSEC",   .reg = "PWR_BDCR",   .raz = false},
    {.bit = "VBSEC",   .reg = "PWR_DBPCR",  .raz = false},
    {.bit = "RETSEC",  .reg = "PWR_IORETR", .raz = false},
    {.bit = "SCMSEC",  .reg = "PWR_SCCR",   .raz = false},
    {.bit = "SCMSEC",  .reg = "PWR_VMCR",   .raz = false},
});

constexpr auto h5_wakeup_fields = std::to_array<pwr_wakeup_field>({
    {.reg = "PWR_WUCR", .prefix = "WUPEN"},
    {.reg = "PWR_WUCR", .prefix = "WUPPUPD"},
    {.reg = "PWR_WUCR", .prefix = "WUPP"},
    {.reg = "PWR_WUSCR", .prefix = "CWUF", .raz = false},
});

constexpr auto h5_system_clock_fields = std::to_array<field_name>({
    {.reg = "PWR_VOSCR", .field = "VOS"},
});

constexpr std::array<write_entry<pwr>, 5> h5_writes{
    {
     {"PWR_PMCR", &pwr::cr1_written},
     {"PWR_VOSCR", &pwr::vosr_written},
     {"PWR_VOSSR", nullptr},
     {"PWR_SECCFGR", nullptr},
     {"PWR_BDCR", &pwr::functions_written},
     }
};

/** @brief STM32L1 PWR, RM0038 5.4: VOS of PWR_CR 01 Range 1, 10 Range 2, 11 Range 3, 00
 *         forbidden, PWR_CSR.VOSF 1 while the regulator changes and 0 when it is ready, the
 *         low-power mode by PDDS. The family has no ACTVOS: the applied range is VOS once VOSF
 *         clears, so the active roles name VOS and VOSF. No booster, no SMPS, no TrustZone. */
constexpr std::array<named_field<pwr_role>, role_count<pwr_role>> l1_names{
    {
     {pwr_role::vos, {"CR", "VOS"}},
     {pwr_role::vos_ready, {"CSR", "VOSF"}},
     {pwr_role::active_vos, {"CR", "VOS"}},
     {pwr_role::active_vos_ready, {"CSR", "VOSF"}},
     {pwr_role::mode, {"CR", "PDDS"}},
     {pwr_role::booster, {"", ""}},
     {pwr_role::booster_ready, {"", ""}},
     {pwr_role::regulator_select, {"", ""}},
     {pwr_role::regulator_status, {"", ""}},
     {pwr_role::io_supply_valid, {"", ""}},
     {pwr_role::usb_supply_valid, {"", ""}},
     }
};

constexpr std::array<std::uint8_t, 4> l1_vos_levels{vos_forbidden, 0, 1, 2};

constexpr std::array<write_entry<pwr>, 2> l1_writes{
    {
     {"CR", &pwr::control_written},
     {"CSR", nullptr},
     }
};

constexpr auto kept_by_system_reset = std::to_array<reset_kind>({
    reset_kind::pin,
    reset_kind::software,
    reset_kind::watchdog,
    reset_kind::window_watchdog,
    reset_kind::low_power,
    reset_kind::option_load,
});

constexpr auto l1_peripheral_reset_kept = std::to_array<field_name>({
    {"CR", "FWU"},
    {"CR", "ULP"},
});

constexpr pwr_variant l1_pwr{
    .fields                = l1_names,
    .modes                 = l1_modes_by_pdds,
    .vos_levels            = l1_vos_levels,
    .vos_ready_set         = false,
    .active_ready_set      = false,
    .stepping              = std::nullopt,
    .smps                  = std::nullopt,
    .secure_register       = {},
    .writes                = l1_writes,
    .trustzone_writes      = {},
    .secured               = {},
    .wakeup_fields         = {},
    .system_clock_fields   = {},
    .peripheral_reset_kept = l1_peripheral_reset_kept,
};

/** @brief STM32L47x/L48x PWR, RM0351 5.4: VOS of PWR_CR1 01 Range 1, 10 Range 2, 00 and 11
 *         forbidden, PWR_SR2.VOSF 1 while the regulator changes, the low-power mode by LPMS.
 *         The family has no ACTVOS: the applied range is VOS once VOSF clears, so the active
 *         roles name VOS and VOSF. No booster, no SMPS regulator select, no TrustZone. */
constexpr std::array<named_field<pwr_role>, role_count<pwr_role>> l4_names{
    {
     {pwr_role::vos, {"CR1", "VOS"}},
     {pwr_role::vos_ready, {"SR2", "VOSF"}},
     {pwr_role::active_vos, {"CR1", "VOS"}},
     {pwr_role::active_vos_ready, {"SR2", "VOSF"}},
     {pwr_role::mode, {"CR1", "LPMS"}},
     {pwr_role::booster, {"", ""}},
     {pwr_role::booster_ready, {"", ""}},
     {pwr_role::regulator_select, {"", ""}},
     {pwr_role::regulator_status, {"", ""}},
     {pwr_role::io_supply_valid, {"CR2", "IOSV"}},
     {pwr_role::usb_supply_valid, {"CR2", "USV"}},
     }
};

constexpr std::array<std::uint8_t, 4> l4_vos_levels{vos_forbidden, 0, 1, vos_forbidden};

constexpr std::array<write_entry<pwr>, 4> l4_writes{
    {
     {"CR1", &pwr::control_written},
     {"CR2", &pwr::functions_written},
     {"CR3", &pwr::functions_written},
     {"SR2", nullptr},
     }
};

constexpr auto l4_unmodelled = std::to_array<field_name>({
    {"CR1", "LPR"  },
    {"CR2", "PVDE" },
    {"CR2", "PVME1"},
    {"CR2", "PVME2"},
    {"CR2", "PVME3"},
    {"CR2", "PVME4"},
    {"CR3", "EWUP1"},
    {"CR3", "EWUP2"},
    {"CR3", "EWUP3"},
    {"CR3", "EWUP4"},
    {"CR3", "EWUP5"},
    {"CR3", "APC"  },
});

constexpr pwr_variant l4_pwr{
    .fields                = l4_names,
    .modes                 = l4_modes_by_lpms,
    .vos_levels            = l4_vos_levels,
    .vos_ready_set         = false,
    .active_ready_set      = false,
    .stepping              = std::nullopt,
    .smps                  = std::nullopt,
    .secure_register       = {},
    .writes                = l4_writes,
    .trustzone_writes      = {},
    .secured               = {},
    .wakeup_fields         = {},
    .system_clock_fields   = {},
    .peripheral_reset_kept = {},
    .unmodelled            = l4_unmodelled,
};

constexpr auto u5_system_reset_kept = std::to_array<field_name>({
    {"PWR_CR3",   "FSTEN"},
    {"PWR_BDCR2", "VBE"  },
    {"PWR_BDCR2", "VBRS" },
});

constexpr auto u5_unmodelled = std::to_array<field_name>({
    {"PWR_BDCR1", "MONEN"},
});

constexpr auto u5_backup_reset_kept = std::to_array<field_name>({
    {"PWR_BDCR1", "BREN" },
    {"PWR_BDCR1", "MONEN"},
});

constexpr auto h5_unmodelled = std::to_array<field_name>({
    {"PWR_BDCR", "MONEN"},
});

constexpr auto h5_system_reset_kept = std::to_array<field_name>({
    {"PWR_SCCR", "BYPASS"},
});

constexpr auto h5_system_reset_words = std::to_array<std::string_view>({"PWR_SCCR"});

constexpr auto h5_backup_reset_kept = std::to_array<field_name>({
    {"PWR_BDCR", "BREN" },
    {"PWR_BDCR", "MONEN"},
    {"PWR_BDCR", "VBE"  },
    {"PWR_BDCR", "VBRS" },
});

constexpr pwr_variant h5_pwr{
    .fields           = h5_names,
    .modes            = h5_modes_by_lpms,
    .vos_levels       = h5_vos_levels,
    .vos_ready_set    = true,
    .active_ready_set = true,
    .stepping =
        vos_stepping{
                     .step   = std::chrono::nanoseconds{10'400},
                     .settle = std::chrono::nanoseconds{15'200},
                     },
    .secure_register       = "PWR_SECCFGR",
    .writes                = h5_writes,
    .trustzone_writes      = h5_trustzone_writes,
    .secured               = h5_secured,
    .wakeup_fields         = h5_wakeup_fields,
    .system_clock_fields   = h5_system_clock_fields,
    .peripheral_reset_kept = {                                         },
    .system_reset_kept     = h5_system_reset_kept,
    .backup_reset_kept     = h5_backup_reset_kept,
    .system_reset_words    = h5_system_reset_words,
    .unmodelled            = h5_unmodelled,
};

constexpr pwr_variant u5_pwr{
    .fields                = u5_names,
    .modes                 = modes_by_lpms,
    .vos_levels            = u5_vos_levels,
    .vos_ready_set         = true,
    .active_ready_set      = true,
    .secure_register       = "PWR_SECCFGR",
    .writes                = u5_writes,
    .trustzone_writes      = u5_trustzone_writes,
    .secured               = u5_secured,
    .wakeup_fields         = u5_wakeup_fields,
    .system_clock_fields   = u5_system_clock_fields,
    .peripheral_reset_kept = {},
    .system_reset_kept     = u5_system_reset_kept,
    .backup_reset_kept     = u5_backup_reset_kept,
    .unmodelled            = u5_unmodelled,
};

/** @brief DS13737 Rev 10 Table 75, typical tSMPS and tLDO at 3 V and 25 C, to the REGS change. */
constexpr regulator_switch u5_regulator_switch{
    .to_smps =
        {std::chrono::microseconds{14}, std::chrono::microseconds{17},
                  std::chrono::microseconds{16}, std::chrono::microseconds{16}},
    .to_ldo =
        {std::chrono::microseconds{16}, std::chrono::microseconds{15},
                  std::chrono::microseconds{14}, std::chrono::microseconds{14}},
};

constexpr pwr_variant u5_smps_pwr = [] {
  auto variant = u5_pwr;
  variant.smps = u5_regulator_switch;
  return variant;
}();

static_assert(names_every_role(u5_names));
static_assert(names_every_role(h5_names));
static_assert(names_every_role(l1_names));
static_assert(names_every_role(l4_names));

pwr::pwr(
    model_context&         ctx,
    peripheral_desc const& desc,
    pwr_variant const&     variant,
    pwr_options const&     options)
    : dispatch_model{ctx, desc, "PWR", variant.writes, variant.trustzone_writes}, options_{options},
      regs_{ctx.regs}, variant_{variant},
      names_{
          variant.fields,
          [this](std::string_view reg_name, std::string_view name) {
            return field(reg_name, name);
          },
          [this](std::string_view reg_name, std::string_view name) {
            return has_field(reg_name, name);
          }},
      unit_{unit_named(ctx.part, desc.name)}
{
  if(not variant.secure_register.empty())
  {
    secure_ = reg(variant.secure_register);
  }
  for(auto const& kept : variant.peripheral_reset_kept)
  {
    peripheral_kept_.push_back(field(kept.reg, kept.field));
  }
  for(auto const& kept : variant.system_reset_kept)
  {
    system_kept_.push_back(field(kept.reg, kept.field));
  }
  for(auto const& kept : variant.backup_reset_kept)
  {
    backup_kept_.push_back(field(kept.reg, kept.field));
    backup_defaults_.push_back(backup_kept_.back().of(describe(kept.reg).reset));
  }
  for(auto const name : variant.system_reset_words)
  {
    system_words_.push_back(reg(name).offset);
  }
  for(auto const& function : variant.unmodelled)
  {
    functions_.push_back(
        {.bits = field(function.reg, function.field), .reg = function.reg, .name = function.field});
  }
  if(ctx.trustzone)
  {
    privilege_            = reg("PWR_PRIVCFGR");
    secure_privileged_    = field("PWR_PRIVCFGR", "SPRIV");
    nonsecure_privileged_ = field("PWR_PRIVCFGR", "NSPRIV");
    add_rules();
  }
}

void pwr::add_rules()
{
  auto const whole = [this](std::string_view name) {
    auto const at = reg(name);
    return field_ref{.offset = at.offset, .lsb = at.lsb, .width = at.width};
  };
  for(auto const& secured : variant_.secured)
  {
    auto const bits =
        secured.field.empty() ? whole(secured.reg) : field(secured.reg, secured.field);
    rules_.push_back(
        {.offset = bits.offset,
         .bits   = bits.mask(),
         .raz    = secured.raz,
         .source = guard_source::own,
         .own    = field("PWR_SECCFGR", secured.bit)});
  }
  for(int x = 1; x <= 8; ++x)
  {
    auto const n   = std::to_string(x);
    auto const bit = field("PWR_SECCFGR", "WUP" + n + "SEC");
    for(auto const& wakeup : variant_.wakeup_fields)
    {
      auto const f = field(wakeup.reg, std::string{wakeup.prefix} + n);
      rules_.push_back(
          {.offset = f.offset,
           .bits   = f.mask(),
           .raz    = wakeup.raz,
           .source = guard_source::own,
           .own    = bit});
    }
  }
  for(auto const& name : variant_.system_clock_fields)
  {
    if(name.optional and not has_field(name.reg, name.field))
    {
      continue;
    }
    auto const f = field(name.reg, name.field);
    rules_.push_back({.offset = f.offset, .bits = f.mask(), .source = guard_source::system_clock});
  }
  auto const ucpd = whole("PWR_UCPDR");
  rules_.push_back(
      {.offset = ucpd.offset,
       .bits   = ucpd.mask(),
       .source = guard_source::peripheral,
       .unit   = unit_named(ctx_.part, "UCPD1")});
  for(char port = 'A'; port <= 'J'; ++port)
  {
    for(auto const& [kind, prefix] : {
            std::pair{"PUCR", "PU"},
            std::pair{"PDCR", "PD"}
    })
    {
      auto const name = std::format("PWR_{}{}", kind, port);
      if(not has_register(name))
      {
        continue;
      }
      for(auto const& bits : describe(name).fields)
      {
        auto const pin = std::string_view{bits.name}.substr(std::string_view{prefix}.size());
        auto const f   = field(name, bits.name);
        rules_.push_back(
            {.offset = f.offset,
             .bits   = f.mask(),
             .source = guard_source::port_pin,
             .peer   = std::format("GPIO{}", port),
             .pin    = static_cast<std::uint8_t>(std::stoi(std::string{pin}))});
      }
    }
  }
  auto const rcc =
      std::ranges::find(ctx_.part.peripherals, std::string_view{"RCC"}, &peripheral_desc::name);
  if(rcc != ctx_.part.peripherals.end())
  {
    if(auto const* const described = find_register(*rcc, "RCC_SECCFGR"))
    {
      auto const sysclk =
          std::ranges::find(described->fields, std::string_view{"SYSCLKSEC"}, &field_desc::name);
      if(sysclk != described->fields.end())
      {
        system_clock_ = field_ref{
            .offset = described->offset & ~3u,
            .lsb    = static_cast<std::uint8_t>(8 * (described->offset % 4) + sysclk->lsb),
            .width  = sysclk->width};
      }
    }
  }
}

auto pwr::rule_secure(pwr_rule const& rule) const -> bool
{
  switch(rule.source)
  {
    case guard_source::own:
      return rule.own.of(regs_.peek(rule.own.offset)) != 0;
    case guard_source::system_clock:
    {
      auto const* const rcc = dynamic_cast<trustzone_aware const*>(ctx_.peers.find("RCC"));
      return rcc != nullptr and system_clock_ and system_clock_->of(rcc->secure_word()) != 0;
    }
    case guard_source::port_pin:
    {
      auto const* const port = dynamic_cast<trustzone_aware const*>(ctx_.peers.find(rule.peer));
      return port != nullptr and (port->secure_word() >> rule.pin & 1u) != 0;
    }
    case guard_source::peripheral:
      return rule.unit and unit_secure(ctx_.peers, ctx_.part, *rule.unit);
  }
  return false;
}

auto pwr::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone and secure_ ? regs_.peek(secure_->offset) : 0;
}

auto pwr::io_supply_valid() const -> bool
{
  auto const valid = names_.find(pwr_role::io_supply_valid);
  return not valid or get(regs_, *valid) != 0;
}

auto pwr::usb_supply_valid() const -> bool
{
  auto const valid = names_.find(pwr_role::usb_supply_valid);
  return not valid or get(regs_, *valid) != 0;
}

void pwr::tell_usb_supply(bool before)
{
  if(usb_supply_valid() == before)
  {
    return;
  }
  if(auto* const usb = dynamic_cast<usb_supply_listener*>(ctx_.peers.find("OTG_FS")))
  {
    usb->usb_supply_changed();
  }
}

auto pwr::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const before = usb_supply_valid();
  auto       result = dispatch_model::on_write(regs, offset, size, old, value, ctx);
  tell_usb_supply(before);
  return result;
}

auto pwr::secure_function() const -> bool
{
  return secure_word() != 0;
}

auto pwr::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  if(write)
  {
    writer_  = ctx.by;
    written_ = word;
  }
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
  auto const spriv     = secure_privileged_.of(privilege) != 0;
  auto const nspriv    = nonsecure_privileged_.of(privilege) != 0;
  bus_guard  out;
  for(auto const& rule : rules_)
  {
    if(rule.offset != word)
    {
      continue;
    }
    auto const secure     = rule_secure(rule);
    auto const refused    = secure and not ctx.secure;
    auto const privileged = (secure ? spriv : nspriv) and not ctx.privileged;
    out                   = out
                          | bus_guard{
                              .raz = (refused and rule.raz) or privileged ? rule.bits : 0,
                              .wi  = refused or privileged ? rule.bits : 0};
  }
  return out;
}

void pwr::reset(register_file& regs, reset_kind kind)
{
  auto const             supplied = usb_supply_valid();
  std::vector<field_ref> keeping;
  if(kind == reset_kind::peripheral)
  {
    keeping.append_range(peripheral_kept_);
  }
  if(std::ranges::contains(kept_by_system_reset, kind))
  {
    keeping.append_range(system_kept_);
  }
  if(kind != reset_kind::power_on)
  {
    keeping.append_range(backup_kept_);
  }
  auto const kept = keeping
                  | std::views::transform(
                        [&](field_ref const& bits) { return bits.of(regs.peek(bits.offset)); })
                  | std::ranges::to<std::vector>();
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  for(auto const& [bits, value] : std::views::zip(keeping, kept))
  {
    regs.poke(bits.offset, bits.with(regs.peek(bits.offset), value));
  }
  state_.settled_level = variant_.vos_levels[get(regs, names_[pwr_role::active_vos])];
  if(kind == reset_kind::peripheral)
  {
    tell_usb_supply(supplied);
  }
}

auto pwr::keeps_over_reset(std::uint32_t offset, reset_kind kind) const -> bool
{
  return std::ranges::contains(kept_by_system_reset, kind)
     and std::ranges::contains(system_words_, offset);
}

void pwr::backup_reset()
{
  for(auto const& [bits, value] : std::views::zip(backup_kept_, backup_defaults_))
  {
    regs_.poke(bits.offset, bits.with(regs_.peek(bits.offset), value));
  }
}

void pwr::vosr_written(std::uint32_t old, std::uint32_t now)
{
  auto const level = names_[pwr_role::vos];
  if(variant_.vos_levels[level.of(now)] == vos_forbidden)
  {
    auto const reg = variant_.fields[std::to_underlying(pwr_role::vos)].name.reg;
    regs_.poke(level.offset, level.with(regs_.peek(level.offset), level.of(old)));
    if(writer_ == initiator::debugger)
    {
      return;
    }
    if(not std::exchange(forbidden_vos_logged_, true))
    {
      warn(
          reg,
          std::format("VOS {:#04b} is forbidden, VOS keeps {:#04b}", level.of(now), level.of(old)));
    }
    if(ctx_.strict_peripherals)
    {
      throw model_error(desc_.name, std::string{reg}, level.offset);
    }
    return;
  }
  if(level.of(now) != level.of(old))
  {
    if(variant_.vos_levels[level.of(now)] < variant_.vos_levels[level.of(old)])
    {
      update(regs_, names_[pwr_role::vos_ready], variant_.vos_ready_set ? 0 : 1);
    }
    update(regs_, names_[pwr_role::active_vos_ready], variant_.active_ready_set ? 0 : 1);
    auto const generation = ++state_.generation[voltage_ready];
    if(variant_.stepping)
    {
      step_active(*variant_.stepping, generation);
    }
    else
    {
      schedule_in(options_.ready_delay, voltage_ready, generation);
    }
  }
  auto const booster = names_.find(pwr_role::booster);
  if(booster and booster->of(now) != booster->of(old))
  {
    update(regs_, names_[pwr_role::booster_ready], 0);
    auto const generation = ++state_.generation[booster_ready];
    if(booster->of(now) != 0)
    {
      schedule_in(options_.ready_delay, booster_ready, generation);
    }
  }
}

void pwr::cr1_written(std::uint32_t, std::uint32_t now)
{
  if(std::exchange(low_power_logged_, true))
  {
    return;
  }
  warn(
      variant_.fields[std::to_underlying(pwr_role::mode)].name.reg,
      std::format(
          "{} selects {}, not modelled: a deep-sleep WFI idles as a plain WFI",
          variant_.fields[std::to_underlying(pwr_role::mode)].name.field,
          reflex::enum_value_name(variant_.modes[names_[pwr_role::mode].of(now)])));
}

void pwr::functions_written(std::uint32_t old, std::uint32_t now)
{
  if(writer_ == initiator::debugger)
  {
    return;
  }
  for(auto& function : functions_)
  {
    if(function.bits.offset != written_ or function.bits.of(old) != 0 or function.bits.of(now) == 0)
    {
      continue;
    }
    if(ctx_.strict_peripherals)
    {
      regs_.poke(written_, old);
    }
    warn_once_or_refuse(
        function.logged, function.reg, written_,
        std::format("{} enables a function the model does not run", function.name));
  }
}

void pwr::control_written(std::uint32_t old, std::uint32_t now)
{
  functions_written(old, now);
  vosr_written(old, now);
  if(names_[pwr_role::mode].of(now) != names_[pwr_role::mode].of(old))
  {
    cr1_written(old, now);
  }
}

void pwr::cr3_written(std::uint32_t old, std::uint32_t now)
{
  auto const select = names_[pwr_role::regulator_select];
  if(not variant_.smps)
  {
    auto const reg = variant_.fields[std::to_underlying(pwr_role::regulator_select)].name.reg;
    if(select.of(old) != 0 or select.of(now) == 0 or writer_ == initiator::debugger)
    {
      return;
    }
    if(not std::exchange(reserved_select_logged_, true))
    {
      warn(reg, "REGSEL selects the SMPS, which this package lacks: REGS stays 0, the LDO");
    }
    if(ctx_.strict_peripherals)
    {
      regs_.poke(select.offset, old);
      throw model_error(desc_.name, std::string{reg}, select.offset);
    }
    return;
  }
  if(select.of(now) == select.of(old))
  {
    return;
  }
  auto const& times = select.of(now) != 0 ? variant_.smps->to_smps : variant_.smps->to_ldo;
  schedule_in(
      times[get(regs_, names_[pwr_role::active_vos])], regulator_ready,
      ++state_.regulator_generation);
}

void pwr::step_active(vos_stepping const& stepping, std::uint64_t generation)
{
  auto const target = get(regs_, names_[pwr_role::vos]);
  auto       active = get(regs_, names_[pwr_role::active_vos]);
  if(active != target)
  {
    active = active < target ? active + 1 : active - 1;
    update(regs_, names_[pwr_role::active_vos], active);
  }
  if(active == target)
  {
    schedule_in(stepping.settle, voltage_ready, generation);
  }
  else
  {
    schedule_in(stepping.step, voltage_step, generation);
  }
}

auto pwr::active_ready() const -> bool
{
  return get(regs_, names_[pwr_role::active_vos_ready]) == (variant_.active_ready_set ? 1u : 0u);
}

auto pwr::active_level() const -> std::uint32_t
{
  if(not active_ready())
  {
    return state_.settled_level;
  }
  return variant_.vos_levels[get(regs_, names_[pwr_role::active_vos])];
}

void pwr::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == regulator_ready)
  {
    if(payload == state_.regulator_generation)
    {
      update(
          regs_, names_[pwr_role::regulator_status],
          get(regs_, names_[pwr_role::regulator_select]));
    }
    return;
  }
  if(kind == voltage_step)
  {
    if(variant_.stepping and payload == state_.generation[voltage_ready])
    {
      step_active(*variant_.stepping, payload);
    }
    return;
  }
  if(kind > booster_ready or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == voltage_ready)
  {
    update(regs_, names_[pwr_role::vos_ready], variant_.vos_ready_set ? 1 : 0);
    update(regs_, names_[pwr_role::active_vos], get(regs_, names_[pwr_role::vos]));
    update(regs_, names_[pwr_role::active_vos_ready], variant_.active_ready_set ? 1 : 0);
    state_.settled_level = variant_.vos_levels[get(regs_, names_[pwr_role::active_vos])];
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const listener = dynamic_cast<voltage_range_listener*>(peer))
      {
        listener->on_voltage_range_change();
      }
    }
  }
  else if(
      auto const booster = names_.find(pwr_role::booster); booster and get(regs_, *booster) != 0)
  {
    update(regs_, names_[pwr_role::booster_ready], 1);
  }
}
}

auto pwr_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, u5_pwr, options);
  };
}

auto pwr_smps_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, u5_smps_pwr, options);
  };
}

auto pwr_l1_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, l1_pwr, options);
  };
}

auto pwr_l4_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, l4_pwr, options);
  };
}

auto pwr_h5_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, h5_pwr, options);
  };
}

namespace
{
constexpr auto pwr_table = std::to_array<builtin>({
    row_of<^^pwr_smps_factory, ^^void, ^^board::pwr>(
        {.ip = "PWR", .ip_version_glob = "STM32U5_smps_pwr", .families = family::u5}),
    row_of<^^pwr_factory, ^^void, ^^board::pwr>(
        {.ip = "PWR", .ip_version_glob = "STM32U5_pwr_*", .families = family::u5}),
    row_of<^^pwr_h5_factory, ^^void, ^^board::pwr>(
        {.ip = "PWR", .ip_version_glob = "STM32H5_pwr_*", .families = family::h5}),
    row_of<^^pwr_l1_factory, ^^void, ^^board::pwr>(
        {.ip = "PWR", .ip_version_glob = "", .families = family::l1}),
    row_of<^^pwr_l4_factory, ^^void, ^^board::pwr>(
        {.ip = "PWR", .ip_version_glob = "", .families = family::l4}),
});
}

auto pwr_rows() -> std::span<builtin const>
{
  return pwr_table;
}
}
