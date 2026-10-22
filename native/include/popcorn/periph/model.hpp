#pragma once

/**
 * @file
 * @brief The peripheral model interface, the context a model is built with, and the registry that
 * selects a model for a peripheral.
 */

#include <popcorn/core/description.hpp>
#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/lines.hpp>
#include <popcorn/periph/register_file.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph
{
/** @brief Bus firewall of a machine, defined in `popcorn/periph/firewall.hpp`. */
struct bus_firewall;

/** @brief Where, when and by whom a guest register access happened. */
struct access_ctx
{
  /** @brief Address the access is attributed to: the start of the translated block that made
   * it, since Unicorn does not tell MMIO callbacks the accessing instruction. Exact, the
   * instruction that caused the access, directly or through a DMA transfer it programmed,
   * while a watch or a trace sink asking for exact pcs is active: the machine then installs a
   * memory hook that syncs the pc, which every guest load and store pays for. */
  address pc;
  /** @brief Instructions executed when the access happened. */
  icount now;
  /** @brief Bus master that made the access. */
  initiator by{initiator::cpu};
  /** @brief Security attribute of the transaction, AHB HNONSEC inverted, whatever window it
   * came through. False for every access, CPU, DMA or debugger, on a machine without TrustZone,
   * `core::machine_options::trustzone` unset. */
  bool secure{};
  /** @brief Whether the transaction is privileged, AHB HPROT[1]: an unprivileged CPU access
   * reads false. Debugger and DMA accesses are privileged unless their master says otherwise. */
  bool privileged{true};
  /** @brief Whether the access came through a secure alias window, a `SEC_` peripheral
   * description with `alias_of` set, or for a memory region access a region with `secure`
   * set. Independent of TrustZone. */
  bool via_secure_alias{};
};

/**
 * @brief Behaviour model of one peripheral instance. Receives reads and writes of the registers
 *        it claims, the register_file serves the rest. Implemented natively or, through a
 *        nanobind trampoline, in Python.
 *
 * Storage: the dispatch never writes the register_file for a claimed register. The model pokes
 * whatever it wants visible there, since register_file::peek() is what Python views, trace and
 * snapshots read. Access values are right-aligned to the access size: a 1-byte read at offset 5
 * returns bits 15:8 of the word at offset 4 in bits 7:0. Neither copyable nor movable.
 */
class POPCORN_API model
{
public:
  /** @brief Creates a model. */
  model() = default;
  /** @brief Not copyable. */
  model(model const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(model const&) -> model& = delete;
  /** @brief Not movable. */
  model(model&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(model&&) -> model& = delete;
  /** @brief Destroys the model. */
  virtual ~model();

  /**
   * @brief Names of the registers the model serves, as the description spells them. Called once,
   *        when the model is attached to its peripheral.
   * @return Register names.
   */
  [[nodiscard]] virtual auto claims() const -> std::vector<std::string> = 0;

  /**
   * @brief Whether a CPU write to this model can change the core clock rate. The machine then
   *        makes the pc of the CPU writes to its registers exact, at the cost of a memory hook
   *        on them, so that a rate change charges the rest of its translated block at the new
   *        rate.
   * @return False by default.
   */
  [[nodiscard]] virtual auto drives_core_clock() const noexcept -> bool
  {
    return false;
  }

  /**
   * @brief Brings the model to its reset state and pokes the reset values of its claimed
   *        registers.
   * @param regs Storage of the peripheral.
   * @param kind Cause of the reset, as the machine received it through
   *        `core::machine_services::request_reset` or chose it at power-up.
   */
  virtual void reset(register_file& regs, reset_kind kind) = 0;

  /**
   * @brief The supply was cut. Called on every model, in description order, before the
   *        power-on reset of `core::machine::power_cycle()`, while the model's state and the
   *        clock are still those of the cut, so `model_context::clock` gives its time. The
   *        default does nothing. A model holding a non-volatile array applies what the cut
   *        leaves of an operation in flight. Never called by reset() nor by a reset a model
   *        requests.
   */
  virtual void power_cut()
  {}

  /**
   * @brief Serves a guest read of a claimed register.
   * @param regs Storage of the peripheral.
   * @param offset Byte offset of the access within the peripheral.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param ctx Where, when and by whom the access happened.
   * @return Value the guest reads, right-aligned to @p size, or a bus error, which the machine
   *         turns into a guest bus fault for a CPU access or a transfer error for a DMA access.
   *         The dispatch masks the value to @p size, bits above it are dropped. A Python model
   *         returning neither an int nor None raises model_error in the dispatch.
   */
  virtual auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> = 0;

  /**
   * @brief Serves a guest write of a claimed register.
   * @param regs Storage of the peripheral.
   * @param offset Byte offset of the access within the peripheral.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param old The accessed bytes as register_file::peek() showed them before the write,
   *        right-aligned to @p size.
   * @param value Value the guest writes, right-aligned to @p size.
   * @param ctx Where, when and by whom the access happened.
   * @return Nothing, or a bus error, which the machine turns into a guest bus fault for a CPU
   *         access or a transfer error for a DMA access. A Python model returning neither None
   *         nor a bool raises model_error in the dispatch.
   */
  virtual auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> = 0;

  /**
   * @brief Serves a CPU or DMA store to a read-only memory region whose description names this
   *        model's peripheral as its controller, such as a word stored into flash while
   *        programming. The region's bytes stay unchanged unless the model writes them, through
   *        core::machine_services with initiator::debugger. Debugger writes bypass it. The
   *        dispatch splits a 64-bit CPU store, VSTR.64 or VSTM of a D register, into two 4-byte
   *        calls, low address first, and skips the second when the first returns a bus error.
   *        The model is called with the engine running and the GIL released.
   * @param region The region the store hit.
   * @param addr Address of the store.
   * @param size Store size in bytes, 1, 2 or 4.
   * @param value Value stored, right-aligned to @p size.
   * @param ctx Where, when and by whom the store happened.
   * @return Nothing when the model took the store, or a bus error, which the machine turns into
   *         an imprecise BusFault, at the end of the current translated block, for a CPU store,
   *         or a transfer error for a DMA store. The default refuses every store with
   *         bus_error::denied.
   */
  virtual auto on_memory_write(
      [[maybe_unused]] region_desc const& region,
      [[maybe_unused]] address            addr,
      [[maybe_unused]] std::uint8_t       size,
      [[maybe_unused]] std::uint32_t      value,
      [[maybe_unused]] access_ctx         ctx) -> std::expected<void, bus_error>
  {
    return std::unexpected{bus_error::denied};
  }

  /**
   * @brief A CPU or DMA load overlapped a range the model watches through
   *        core::machine_services::watch_reads. Called once per load, also when the call
   *        changes the model's watches, before the load reads its bytes, on the machine thread
   *        with the engine running and the GIL released. An interrupt the call makes
   *        deliverable is taken before the next instruction. The default does nothing.
   * @param region Region the load hit, an alias included.
   * @param addr Address loaded, as the instruction or channel spelled it.
   * @param size Load size in bytes, 8 for a 64-bit load.
   * @param ctx Where, when and by whom the load happened.
   */
  virtual void on_memory_read(
      [[maybe_unused]] region_desc const& region,
      [[maybe_unused]] address            addr,
      [[maybe_unused]] std::uint8_t       size,
      [[maybe_unused]] access_ctx         ctx)
  {}

  /**
   * @brief Handles an event the model scheduled, when virtual time reaches it.
   * @param kind Event kind the model chose when scheduling, the `kind` of the event id.
   * @param payload Payload the model passed when scheduling.
   */
  virtual void on_event(std::uint32_t kind, std::uint64_t payload) = 0;

  /**
   * @brief Appends the model state to a snapshot.
   * @param out Snapshot bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores the model state from bytes save() wrote.
   * @param in Bytes of one save() call.
   */
  virtual void load(std::span<std::byte const> in) = 0;

  /**
   * @brief Whether the model keeps no state beyond its register storage. A snapshot holds no
   *        bytes for such a model, so save() and load() are never called for it, and it restores
   *        from the register storage alone. A save made by a machine with no model for the
   *        peripheral then loads too. A snapshot missing the bytes of any other model is refused.
   * @return False by default.
   */
  [[nodiscard]] virtual auto stateless() const -> bool
  {
    return false;
  }

  /**
   * @brief Whether a register the model claims is storage it does not model. A host view,
   *        core::memory_view and the peripheral views built on it, refuses such a register with
   *        model_error, as a register of a peripheral without a model. peek and poke still reach
   *        it.
   * @param offset Byte offset of the register's word in the peripheral, a multiple of 4.
   * @return False by default.
   */
  [[nodiscard]] virtual auto storage_only([[maybe_unused]] std::uint32_t offset) const -> bool
  {
    return false;
  }
};

/**
 * @brief Models of one machine, by peripheral instance name. Empty while the machine builds its
 *        models, factories and model::claims() included, filled once every model is attached.
 *        Never used from a model destructor.
 */
class POPCORN_API peer_directory
{
public:
  /** @brief Creates the directory. */
  peer_directory() = default;
  /** @brief Not copyable, contexts hold its address. */
  peer_directory(peer_directory const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(peer_directory const&) -> peer_directory& = delete;
  /** @brief Not movable. */
  peer_directory(peer_directory&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(peer_directory&&) -> peer_directory& = delete;

  /**
   * @brief Model of a peripheral of the same machine.
   * @param peripheral Instance name as the description spells it, an alias names the model of
   *        the peripheral it aliases.
   * @return The model, or null when the peripheral has none, is not in the description, or the
   *         machine is still building its models.
   */
  [[nodiscard]] virtual auto find(std::string_view peripheral) const noexcept -> model* = 0;

  /**
   * @brief Every model of the machine, in description order, empty while the machine is still
   *        building its models.
   */
  [[nodiscard]] virtual auto all() const noexcept -> std::span<model* const> = 0;

protected:
  /** @brief Destroys the directory, only through its implementation. */
  virtual ~peer_directory();
};

/**
 * @brief What a model factory hands a new model. The context and everything it references
 *        outlive the model, so the model may keep the reference.
 */
struct POPCORN_API model_context
{
  /** @brief Storage of the peripheral. */
  register_file& regs;
  /** @brief IRQ lines of the peripheral, in the order of popcorn::irq_sources(): its own, then
   * under TrustZone those of its Secure aliases. */
  std::span<irq_line> irqs;
  /** @brief Line to the core's non-maskable interrupt, level-sensitive and shared by every
   * model: the input is active while any model holds its line active. An NMI still active when
   * its handler returns is taken again. Ignored by an architecture without an NMI input. */
  irq_line& nmi;
  /** @brief DMA request lines of the peripheral, in description order. */
  std::span<dma_request> dma;
  /** @brief The model's own access to the event queue, stamped with its index. */
  core::event_port& events;
  /** @brief Virtual clock of the machine. */
  core::clock& clock;
  /** @brief Machine services: deferred stop, guest bus access. */
  core::machine_services& services;
  /** @brief Models of the same machine, see peer_directory for when it is filled. */
  peer_directory& peers;
  /** @brief Description of the machine, valid for the model's life. */
  part_description const& part;
  /** @brief Whether the machine runs with `core::machine_options::strict_peripherals`, fixed for
   * the machine's life, so a model may cache it: a model may raise model_error where it would
   * only warn. */
  bool strict_peripherals{};
  /** @brief Whether the machine runs with `core::machine_options::trustzone`, TZEN=1 on an STM32,
   * fixed for the machine's life. */
  bool trustzone{};

  /**
   * @brief Installs the machine's bus firewall, forwarding to services. Only from the model's
   *        factory or constructor: the machine asks it before every access to a peripheral
   *        window from then on.
   * @param firewall Firewall, which must live as long as the model.
   * @throws model_error naming the peripheral being built when the machine already has a
   *         firewall, or when the machine no longer builds its models. The machine also raises
   *         it, naming the peripheral, when a factory installs a firewall and returns no model.
   */
  void install_firewall(bus_firewall& firewall);

  /**
   * @brief Asks the run loop to stop, forwarding to services. Deferred: it only records the
   *        request, the run loop stops at the next block boundary, and nothing calls
   *        `uc_emu_stop` from the model's callback.
   * @param reason Reason the run reports.
   * @param detail Human-readable detail for the stop record.
   */
  void request_stop(stop_reason reason, std::string detail);
};

/**
 * @brief Creates a model for one peripheral instance. Shared ownership, so a Python model stays
 *        alive through its Python instance for as long as the machine holds it.
 */
using model_factory =
    std::copyable_function<std::shared_ptr<model>(model_context& ctx, peripheral_desc const& desc)>;

/**
 * @brief Registers a model factory for every peripheral of an IP.
 * @param ip IP name, as the description's `ip` spells it.
 * @param ip_version_glob Glob over the description's `ip_version`, `*` matches any.
 * @param factory Factory called once per matching peripheral instance.
 */
POPCORN_API void
    register_model(std::string_view ip, std::string_view ip_version_glob, model_factory factory);

/**
 * @brief Matches a glob with `*` and `?` against a text, as the model registry matches IP
 *        versions.
 * @param pattern Glob.
 * @param text Text.
 */
[[nodiscard]] POPCORN_API auto glob_match(std::string_view pattern, std::string_view text) noexcept
    -> bool;

/**
 * @brief Glob of the registration that serves a peripheral of an IP and IP version: the latest
 *        whose IP equals @p ip and whose glob matches @p ip_version.
 * @param ip IP name.
 * @param ip_version IP version.
 * @return The glob, empty when no registration matches.
 */
[[nodiscard]] POPCORN_API auto registered_glob(std::string_view ip, std::string_view ip_version)
    -> std::optional<std::string>;
}
