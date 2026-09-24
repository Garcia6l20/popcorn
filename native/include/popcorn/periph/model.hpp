#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/lines.hpp>
#include <popcorn/periph/register_file.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph
{
/** @brief Where, when and by whom a guest register access happened. */
struct access_ctx
{
  /** @brief Address of the instruction that caused the access, directly or through a DMA
   * transfer it programmed. */
  address pc;
  /** @brief Instructions executed when the access happened. */
  icount now;
  /** @brief Bus master that made the access. */
  initiator by{initiator::cpu};
};

/**
 * @brief Cause of a reset. Backup-domain state, such as an RTC and its backup registers,
 *        survives every kind except power_on.
 */
enum class[[= reflex::derive{reflex::Format}]] reset_kind
{
  power_on,
  brown_out,
  pin,
  software,
  watchdog,
  window_watchdog,
  low_power,
  option_load,
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
class model
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
  virtual ~model() = default;

  /**
   * @brief Names of the registers the model serves, as the description spells them. Called once,
   *        when the model is attached to its peripheral.
   * @return Register names.
   */
  [[nodiscard]] virtual auto claims() const -> std::vector<std::string> = 0;

  /**
   * @brief Brings the model to its reset state and pokes the reset values of its claimed
   *        registers.
   * @param regs Storage of the peripheral.
   * @param kind Cause of the reset.
   */
  virtual void reset(register_file& regs, reset_kind kind) = 0;

  /**
   * @brief Serves a guest read of a claimed register.
   * @param regs Storage of the peripheral.
   * @param offset Byte offset of the access within the peripheral.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param ctx Where, when and by whom the access happened.
   * @return Value the guest reads, right-aligned to @p size.
   */
  virtual auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::uint32_t = 0;

  /**
   * @brief Serves a guest write of a claimed register.
   * @param regs Storage of the peripheral.
   * @param offset Byte offset of the access within the peripheral.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param old The accessed bytes as register_file::peek() showed them before the write,
   *        right-aligned to @p size.
   * @param value Value the guest writes, right-aligned to @p size.
   * @param ctx Where, when and by whom the access happened.
   */
  virtual void on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) = 0;

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
};

/**
 * @brief What a model factory hands a new model. The context and everything it references
 *        outlive the model, so the model may keep the reference.
 */
struct model_context
{
  /** @brief Storage of the peripheral. */
  register_file& regs;
  /** @brief IRQ lines of the peripheral, in description order. */
  std::span<irq_line> irqs;
  /** @brief DMA request lines of the peripheral, in description order. */
  std::span<dma_request> dma;
  /** @brief Event queue of the machine. */
  core::scheduler& sched;
  /** @brief Virtual clock of the machine. */
  core::clock& clock;
  /** @brief Machine services: deferred stop, guest bus access. */
  core::machine_services& services;

  /**
   * @brief Asks the run loop to stop, forwarding to services. Deferred: it only records the
   *        request, the run loop stops at the next instruction boundary, and nothing calls
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
    std::function<std::shared_ptr<model>(model_context& ctx, peripheral_desc const& desc)>;

/**
 * @brief Registers a model factory for every peripheral of an IP.
 * @param ip IP name, as the description's `ip` spells it.
 * @param ip_version_glob Glob over the description's `ip_version`, `*` matches any.
 * @param factory Factory called once per matching peripheral instance.
 */
void register_model(std::string_view ip, std::string_view ip_version_glob, model_factory factory);
}
