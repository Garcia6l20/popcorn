#pragma once

/**
 * @file
 * @brief The services a machine offers to its architecture module and models: guest bus access,
 * deferred stops and resets, and the helpers that check an access.
 */

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph
{
/** @brief Bus firewall of a machine, defined in `popcorn/periph/firewall.hpp`. */
struct bus_firewall;
}

namespace popcorn::core
{
/**
 * @brief Whether a value fits in an access of a given size, right-aligned.
 * @param value Access value.
 * @param size Access size in bytes, 1, 2 or 4.
 */
constexpr auto fits_access(std::uint32_t value, std::uint8_t size) noexcept -> bool
{
  return size == 4 or value < (std::uint32_t{1} << (8u * size));
}

/**
 * @brief Whether a block of bytes at a guest address stays within the 32-bit address space.
 * @param addr First guest address.
 * @param size Block size in bytes.
 */
constexpr auto fits_address_space(address addr, std::size_t size) noexcept -> bool
{
  return size <= std::size_t{0x1'0000'0000} - addr;
}

/**
 * @brief Size of the next bus access when a block is split: the largest naturally aligned access
 *        of 4, 2 or 1 bytes at @p addr that does not pass the end of the block.
 * @param addr Guest address of the next access.
 * @param remaining Bytes left in the block, at least 1.
 * @return 4, 2 or 1.
 */
constexpr auto block_access_size(address addr, std::size_t remaining) noexcept -> std::uint8_t
    pre(remaining > 0)
{
  if(addr % 4 == 0 and remaining >= 4)
  {
    return 4;
  }
  if(addr % 2 == 0 and remaining >= 2)
  {
    return 2;
  }
  return 1;
}

/** @brief Why a block access through machine_services stopped. */
struct block_error
{
  /** @brief Why the bus refused the failing access. */
  bus_error error{};
  /** @brief First address of the failing access. Bytes before it were transferred. */
  address at{};

  /** @brief Member-wise equality. */
  friend auto operator==(block_error const&, block_error const&) -> bool = default;
};

/**
 * @brief Firewall decision a host bus master keeps across its accesses, such as a DMA channel
 *        during one transfer: the addresses [first, last] of one memory region where the bus
 *        firewall allows every access with the attributes and operation the grant was made for.
 *        The machine fills it on an allowed access and skips the firewall for a later access
 *        whose bytes, address to address plus size, all lie inside it, until a memory policy
 *        change, a restore or a reset makes it stale. The holder drops it, `grant = {}`, when
 *        its attributes change. Default constructed it grants nothing.
 */
struct memory_grant
{
  /** @brief Policy generation the grant was made in, 0 for none. */
  std::uint64_t generation{};
  /** @brief First granted address. */
  address first{};
  /** @brief Last granted address, included. */
  address last{};
};

/**
 * @brief What the machine decided at an instruction the arch module emulates,
 *        machine_services::emulated_instruction(). `proceed`: counted, probed and covered, the
 *        module applies the instruction. `returned`: an intercept returned to the caller
 *        through arch::return_to_caller(), the module skips the instruction and completes
 *        arch::return_interrupt() itself, nothing counted. `stopped`: the run ends before the
 *        instruction, at a slice boundary or an intercept stop, the module leaves the core at
 *        it and resumes, then the trap hook ends the engine run, nothing counted.
 */
enum class emulated_arrival : std::uint8_t
{
  proceed,
  returned,
  stopped,
};

/**
 * @brief What the machine offers the arch module and every peripheral model beyond the clock and
 *        their event port: deferred stop, reset and slice end, guest bus access and the trace
 *        ring. Implemented by the machine, and by fakes in tests. Callers reach it through
 *        `periph::model_context` or `arch::arch_context`.
 *
 * The public functions check their contracts and forward to the private virtual ones, which an
 * implementation overrides: a virtual function cannot carry a contract. Every request but
 * watch_reads() and emulated_instruction() is deferred: it only records, the next block hook
 * ends the slice at an instruction boundary, and nothing calls `uc_emu_stop` from the caller's
 * callback, so every function is safe from MMIO, memory, instruction and interrupt hooks.
 * watch_reads() takes effect at once and is safe from the same callbacks. Neither copyable nor
 * movable, contexts hold references to it.
 */
class POPCORN_API machine_services
{
public:
  /** @brief Creates the services. */
  machine_services() = default;
  /** @brief Not copyable. */
  machine_services(machine_services const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(machine_services const&) -> machine_services& = delete;
  /** @brief Not movable. */
  machine_services(machine_services&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(machine_services&&) -> machine_services& = delete;
  /** @brief Destroys the services. */
  virtual ~machine_services();

  /**
   * @brief Asks the run loop to stop and report @p reason. Deferred, callable from any thread:
   *        a request no slice took yet stops the next one.
   * @param reason Reason the run reports.
   * @param detail Human-readable detail for the stop record.
   */
  void request_stop(stop_reason reason, std::string detail) pre(is_enumerator(reason))
  {
    do_request_stop(reason, std::move(detail));
  }

  /**
   * @brief Asks the machine to reset, as Arm SYSRESETREQ or a watchdog does. Deferred: the slice
   *        ends, then the machine resets the arch module and passes @p kind to every model's
   *        `periph::model::reset`.
   * @param kind Cause of the reset.
   */
  void request_reset(reset_kind kind) pre(is_enumerator(kind))
  {
    do_request_reset(kind);
  }

  /**
   * @brief Holds a peripheral in a reset a bus reset bit drives, such as an STM32 RCC_xxxRSTR
   *        bit, or releases it. Applies at once. Entering the hold returns every register no
   *        model claims to its reset value and opens its write-once locks, as a machine reset
   *        does for that peripheral. While held, a bus write to such a register is ignored,
   *        from every bus initiator, after the bus firewall, and leaves a trace record as a
   *        firewall-ignored write does. `periph::register_file::poke` is not a bus access and
   *        bypasses the hold. A snapshot keeps the hold. A machine reset releases it.
   * @param peripheral Instance name as the description spells it.
   * @param held True while the reset bit is 1.
   * @throws config_error for a name the description lacks, or from an implementation without
   *         peripherals.
   */
  void hold_in_reset(std::string_view peripheral, bool held)
  {
    do_hold_in_reset(peripheral, held);
  }

  /**
   * @brief Ends the current slice without a stop report. Deferred. An arch-private hook, such as
   *        the Arm WFI instruction hook, calls it after entering the idle state. A slice that
   *        ends with `arch::idle()` true and no stop request moves the clock to the next event.
   */
  void end_slice()
  {
    do_end_slice();
  }

  /**
   * @brief From a memory access callback during a run: ends the engine run right after the
   *        instruction being executed, so the interrupt an access of that instruction made
   *        deliverable is taken before the next one, as the hardware does. For the arch's own
   *        memory windows, such as the Arm SCS, after a store that pends or unmasks an
   *        exception. Inside a Thumb IT block the run ends after the block. Peripheral model
   *        accesses need not call it: the machine checks them itself. No effect anywhere else,
   *        such as a debugger access between slices.
   */
  void end_after_instruction()
  {
    do_end_after_instruction();
  }

  /**
   * @brief Reads guest memory or a peripheral register as a bus master. Memory, MMIO dispatch,
   *        the bus firewall and watches see the access with @p attrs, exactly as a guest access.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param attrs Bus master and transaction attributes of the access.
   * @return Value right-aligned to @p size, or why the bus refused the access.
   */
  auto read(address addr, std::uint8_t const size, bus_attrs attrs)
      -> std::expected<std::uint32_t, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0) post(result : not result.has_value() or fits_access(*result, size))
  {
    return do_read(addr, size, attrs);
  }

  /**
   * @brief Reads as read() with attributes, through a grant: an access inside a current
   *        @p grant skips the bus firewall, another access asks it and refills or drops the
   *        grant. The result is what read() gives.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param attrs Bus master and transaction attributes of the access, those the grant was made
   *        for.
   * @param grant Grant of the caller, kept for these attributes and reads only.
   * @return Value right-aligned to @p size, or why the bus refused the access.
   */
  auto read(address addr, std::uint8_t const size, bus_attrs attrs, memory_grant& grant)
      -> std::expected<std::uint32_t, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0) post(result : not result.has_value() or fits_access(*result, size))
  {
    return do_read_granted(addr, size, attrs, grant);
  }

  /**
   * @brief Reads as read() with attributes, as a Secure privileged access made by @p by.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param by Bus master making the access.
   * @return Value right-aligned to @p size, or why the bus refused the access.
   */
  auto read(address addr, std::uint8_t const size, initiator by)
      -> std::expected<std::uint32_t, bus_error>
  {
    return read(addr, size, bus_attrs{.by = by});
  }

  /**
   * @brief Writes guest memory or a peripheral register as a bus master, seen by memory, MMIO
   *        dispatch, the bus firewall and watches with @p attrs.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param value Value right-aligned to @p size.
   * @param attrs Bus master and transaction attributes of the access.
   * @return Nothing, or why the bus refused the access.
   */
  auto write(address addr, std::uint8_t size, std::uint32_t value, bus_attrs attrs)
      -> std::expected<void, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0) pre(fits_access(value, size))
  {
    return do_write(addr, size, value, attrs);
  }

  /**
   * @brief Writes as write() with attributes, through a grant, as read() with a grant does.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param value Value right-aligned to @p size.
   * @param attrs Bus master and transaction attributes of the access, those the grant was made
   *        for.
   * @param grant Grant of the caller, kept for these attributes and writes only.
   * @return Nothing, or why the bus refused the access.
   */
  auto write(
      address       addr,
      std::uint8_t  size,
      std::uint32_t value,
      bus_attrs     attrs,
      memory_grant& grant)
      -> std::expected<void, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0) pre(fits_access(value, size))
  {
    return do_write_granted(addr, size, value, attrs, grant);
  }

  /**
   * @brief Writes as write() with attributes, as a Secure privileged access made by @p by.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param value Value right-aligned to @p size.
   * @param by Bus master making the access.
   * @return Nothing, or why the bus refused the access.
   */
  auto write(address addr, std::uint8_t size, std::uint32_t value, initiator by)
      -> std::expected<void, bus_error>
  {
    return write(addr, size, value, bus_attrs{.by = by});
  }

  /**
   * @brief Reads a block of guest bytes as a bus master, for DMA and debugger copies of memory.
   *        Unaligned addresses and sizes are allowed. The machine splits the block into bus
   *        accesses in ascending address order, each of block_access_size(), so MMIO dispatch and
   *        watches see each access with @p attrs. A FIFO or other register with read side
   *        effects is read with read(), never as a block, since the split decides how many
   *        accesses it sees.
   * @param addr First guest address.
   * @param out Bytes read, in address order.
   * @param attrs Bus master and transaction attributes of every access.
   * @return Nothing, or the failing access. The bytes of @p out before `at` are valid.
   */
  auto read_block(address const addr, std::span<std::byte> const out, bus_attrs attrs)
      -> std::expected<void, block_error> pre(fits_address_space(addr, out.size())) post(
          result : result.has_value()
          or (result.error().at >= addr and result.error().at - addr < out.size()))
  {
    return do_read_block(addr, out, attrs);
  }

  /**
   * @brief Reads a block as read_block() with attributes, as Secure privileged accesses made
   *        by @p by.
   * @param addr First guest address.
   * @param out Bytes read, in address order.
   * @param by Bus master making the accesses.
   * @return Nothing, or the failing access. The bytes of @p out before `at` are valid.
   */
  auto read_block(address const addr, std::span<std::byte> const out, initiator by)
      -> std::expected<void, block_error>
  {
    return read_block(addr, out, bus_attrs{.by = by});
  }

  /**
   * @brief Writes a block of guest bytes as a bus master, for DMA and debugger copies of memory.
   *        Unaligned addresses and sizes are allowed, split as for read_block(). A FIFO or other
   *        register with write side effects is written with write(), never as a block.
   * @param addr First guest address.
   * @param in Bytes to write, in address order.
   * @param attrs Bus master and transaction attributes of every access.
   * @return Nothing, or the failing access. The bytes before `at` were written.
   */
  auto write_block(address const addr, std::span<std::byte const> const in, bus_attrs attrs)
      -> std::expected<void, block_error> pre(fits_address_space(addr, in.size())) post(
          result : result.has_value()
          or (result.error().at >= addr and result.error().at - addr < in.size()))
  {
    return do_write_block(addr, in, attrs);
  }

  /**
   * @brief Writes a block as write_block() with attributes, as Secure privileged accesses made
   *        by @p by.
   * @param addr First guest address.
   * @param in Bytes to write, in address order.
   * @param by Bus master making the accesses.
   * @return Nothing, or the failing access. The bytes before `at` were written.
   */
  auto write_block(address const addr, std::span<std::byte const> const in, initiator by)
      -> std::expected<void, block_error>
  {
    return write_block(addr, in, bus_attrs{.by = by});
  }

  /**
   * @brief Installs the machine's bus firewall, which MMIO dispatch asks before every access to
   *        a peripheral window. Called through periph::model_context::install_firewall.
   * @param firewall Firewall, which must outlive every access of the machine.
   * @throws model_error when the machine already has a firewall or no longer builds its
   *         models, and config_error from an implementation without a firewall.
   */
  void install_firewall(periph::bus_firewall& firewall)
  {
    do_install_firewall(firewall);
  }

  /**
   * @brief From the model that owns the bus firewall, after a change of its memory policy, a
   *        periph::bus_firewall::memory_page verdict: drops every page verdict the engine keeps,
   *        so the next access of each page asks the firewall again. A data verdict then holds
   *        from the next access. Code already running inside a page whose fetch verdict changed
   *        runs on until the engine run ends, so the owner also calls end_after_instruction()
   *        after a fetch-verdict change. It also retires the code the engine translated in pages
   *        whose fetch verdict is per fetch window, page_verdict::fetch_window_shift: without it
   *        such code keeps running under the old verdicts. Every page refills after it, so call
   *        it only when a verdict changes. Safe from a register access during a run and between
   *        slices.
   */
  void memory_policy_changed()
  {
    do_memory_policy_changed();
  }

  /**
   * @brief Starts or stops reporting CPU and DMA loads of [@p at, @p at + @p size) to the
   *        controller model of the region holding it, through periph::model::on_memory_read.
   *        Covers every alias of the bytes. Idempotent per range. Host and debugger reads are
   *        never reported, nor are instruction fetches, vector reads and exception stacking.
   *        Unlike the other requests it is not deferred: it takes effect at once, and is safe
   *        from model events and memory hooks, an on_memory_read call included. Each range
   *        spans from 7 bytes before it to its end. Every engine page a span touches gets one
   *        engine read hook per alias of the page, shared by every range on it, so a page costs
   *        the same whatever the number of ranges. Every load starting on such a page takes
   *        the slow path and calls the hook. Every slow-path load of the machine, MMIO reads
   *        included, walks every engine read hook, about 1 ns each. A DMA block read copies at
   *        once unless it reaches a watched range. Other loads and every fetch are unaffected.
   * @param at First address, inside a region with a controller.
   * @param size Bytes, the range inside that region.
   * @param on Start or stop.
   * @throws config_error when the range is not inside one region with a controller model, and
   *         from an implementation without controller regions.
   */
  void watch_reads(address const at, std::uint32_t const size, bool on) pre(size > 0)
      pre(fits_address_space(at, size))
  {
    do_watch_reads(at, size, on);
  }

  /**
   * @brief Appends a record to the machine's always-on trace ring.
   * @param record Record to append.
   */
  void trace(trace_record const& record)
  {
    do_trace(record);
  }

  /**
   * @brief Sends a record to the machine's log sinks, those of machine::add_log_sink, as the
   *        core's own records. A model fills `peripheral` with its peripheral's name and usually
   *        `kind` log_kind::model_warning. Logging once per condition is the caller's choice.
   * @param record Record to log. A `pc` of 0 is replaced by the current pc.
   */
  void log(log_record record)
  {
    do_log(std::move(record));
  }

  /**
   * @brief From the arch module's trap callback, for an instruction it emulates with no engine
   *        block, such as the Arm SG of a Non-secure call: first ends the run there when the
   *        block hook would end it before an instruction, then runs the probes, intercepts and
   *        return hooks of @p at as the engine code hook would, then counts one instruction and
   *        its coverage. Not deferred. Call it once per arrival, with the pc at @p at.
   * @param at Address of the instruction.
   * @return What the module does with the instruction. The default counts nothing and
   *         proceeds.
   */
  auto emulated_instruction(address at) -> emulated_arrival
  {
    return do_emulated_instruction(at);
  }

private:
  virtual void do_request_stop(stop_reason reason, std::string detail) = 0;
  virtual void do_request_reset(reset_kind kind)                       = 0;
  virtual void do_end_slice()                                          = 0;
  virtual void do_end_after_instruction()                              = 0;
  virtual auto do_read(address addr, std::uint8_t size, bus_attrs attrs)
      -> std::expected<std::uint32_t, bus_error> = 0;
  virtual auto do_write(address addr, std::uint8_t size, std::uint32_t value, bus_attrs attrs)
      -> std::expected<void, bus_error> = 0;
  virtual auto
      do_read_granted(address addr, std::uint8_t size, bus_attrs attrs, memory_grant& grant)
          -> std::expected<std::uint32_t, bus_error>;
  virtual auto do_write_granted(
      address       addr,
      std::uint8_t  size,
      std::uint32_t value,
      bus_attrs     attrs,
      memory_grant& grant) -> std::expected<void, bus_error>;
  virtual auto do_read_block(address addr, std::span<std::byte> out, bus_attrs attrs)
      -> std::expected<void, block_error> = 0;
  virtual auto do_write_block(address addr, std::span<std::byte const> in, bus_attrs attrs)
      -> std::expected<void, block_error>           = 0;
  virtual void do_trace(trace_record const& record) = 0;
  virtual void do_log(log_record record)            = 0;
  virtual void do_watch_reads(address at, std::uint32_t size, bool on);
  virtual void do_hold_in_reset(std::string_view peripheral, bool held);
  virtual void do_install_firewall(periph::bus_firewall& firewall);
  virtual void do_memory_policy_changed();
  virtual auto do_emulated_instruction(address at) -> emulated_arrival;
};
}
