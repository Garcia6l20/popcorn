#pragma once

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>

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
  friend auto operator==(block_error, block_error) -> bool = default;
};

/**
 * @brief What the machine offers the arch module and every peripheral model beyond the clock and
 *        their event port: deferred stop, reset and slice end, guest bus access and the trace
 *        ring. Implemented by the machine, and by fakes in tests. Callers reach it through
 *        `periph::model_context` or `arch::arch_context`.
 *
 * The public functions check their contracts and forward to the private virtual ones, which an
 * implementation overrides: a virtual function cannot carry a contract. Every request is
 * deferred: it only records, the next block hook ends the slice at an instruction boundary, and
 * nothing calls `uc_emu_stop` from the caller's callback, so every function is safe from MMIO,
 * memory, instruction and interrupt hooks. Neither copyable nor movable, contexts hold
 * references to it.
 */
class machine_services
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
   * @brief Asks the run loop to stop and report @p reason. Deferred.
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
   * @brief Reads guest memory or a peripheral register as a bus master. Memory, MMIO dispatch
   *        and watches see the access as made by @p by, exactly as a guest access.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param by Bus master making the access.
   * @return Value right-aligned to @p size, or why the bus refused the access.
   */
  auto read(address addr, std::uint8_t const size, initiator by)
      -> std::expected<std::uint32_t, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0) post(result : not result.has_value() or fits_access(*result, size))
  {
    return do_read(addr, size, by);
  }

  /**
   * @brief Writes guest memory or a peripheral register as a bus master, seen by memory, MMIO
   *        dispatch and watches as made by @p by.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param value Value right-aligned to @p size.
   * @param by Bus master making the access.
   * @return Nothing, or why the bus refused the access.
   */
  auto write(address addr, std::uint8_t size, std::uint32_t value, initiator by)
      -> std::expected<void, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0) pre(fits_access(value, size))
  {
    return do_write(addr, size, value, by);
  }

  /**
   * @brief Reads a block of guest bytes as a bus master, for DMA and debugger copies of memory.
   *        Unaligned addresses and sizes are allowed. The machine splits the block into bus
   *        accesses in ascending address order, each of block_access_size(), so MMIO dispatch and
   *        watches see each access. A FIFO or other register with read side effects is read with
   *        read(), never as a block, since the split decides how many accesses it sees.
   * @param addr First guest address.
   * @param out Bytes read, in address order.
   * @param by Bus master making the accesses.
   * @return Nothing, or the failing access. The bytes of @p out before `at` are valid.
   */
  auto read_block(address const addr, std::span<std::byte> const out, initiator by)
      -> std::expected<void, block_error> pre(fits_address_space(addr, out.size())) post(
          result : result.has_value()
          or (result.error().at >= addr and result.error().at - addr < out.size()))
  {
    return do_read_block(addr, out, by);
  }

  /**
   * @brief Writes a block of guest bytes as a bus master, for DMA and debugger copies of memory.
   *        Unaligned addresses and sizes are allowed, split as for read_block(). A FIFO or other
   *        register with write side effects is written with write(), never as a block.
   * @param addr First guest address.
   * @param in Bytes to write, in address order.
   * @param by Bus master making the accesses.
   * @return Nothing, or the failing access. The bytes before `at` were written.
   */
  auto write_block(address const addr, std::span<std::byte const> const in, initiator by)
      -> std::expected<void, block_error> pre(fits_address_space(addr, in.size())) post(
          result : result.has_value()
          or (result.error().at >= addr and result.error().at - addr < in.size()))
  {
    return do_write_block(addr, in, by);
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

private:
  virtual void do_request_stop(stop_reason reason, std::string detail) = 0;
  virtual void do_request_reset(reset_kind kind)                       = 0;
  virtual void do_end_slice()                                          = 0;
  virtual void do_end_after_instruction()                              = 0;
  virtual auto do_read(address addr, std::uint8_t size, initiator by)
      -> std::expected<std::uint32_t, bus_error> = 0;
  virtual auto do_write(address addr, std::uint8_t size, std::uint32_t value, initiator by)
      -> std::expected<void, bus_error> = 0;
  virtual auto do_read_block(address addr, std::span<std::byte> out, initiator by)
      -> std::expected<void, block_error> = 0;
  virtual auto do_write_block(address addr, std::span<std::byte const> in, initiator by)
      -> std::expected<void, block_error>           = 0;
  virtual void do_trace(trace_record const& record) = 0;
  virtual void do_log(log_record record)            = 0;
};
}
