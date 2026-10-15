#pragma once

#include "access_op.hpp"
#include "alive.hpp"
#include "gil_locks.hpp"
#include "link_queues.hpp"
#include "pty.hpp"

#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/audio.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/uart.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief One frame the firmware transmitted on a UART. */
struct[[= py::rename{"UartFrame"}]] uart_frame
{
  /** @brief Data bits. */
  [[= py::readonly]] std::uint16_t data{};
  /** @brief Virtual time its stop bit ended, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
  /** @brief Whether it was a break, data 0. */
  [[= py::readonly]] bool brk{};
};

/**
 * @brief A UART of the machine connected to Python: frames the firmware transmits are queued
 *        with their time, frames Python sends enter the receiver at the configured baud. Valid
 *        while the machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"UartQueue"}]] uart_queue
{
public:
  /**
   * @brief Connects a queue to a port, replacing its far end.
   * @param port The UART.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, through which send() and queued() reach the UART.
   */
  [[= py::skip]] uart_queue(
      periph::uart_port& port,
      detail::alive_flag alive,
      detail::host_ref   host);

  /**
   * @brief Queues frames towards the firmware, one frame time apart.
   * @param data Frames, one per byte.
   */
  [[= py::doc{"Queue bytes towards the firmware, one frame each."}]] void
      send(reflex::py::nb::bytes const& data);

  /** @brief Frames sent and not yet delivered to the receiver. */
  [[= py::doc{"Frames sent and not yet delivered to the receiver."}]] [[nodiscard]] auto
      queued() const -> std::uint64_t;

  /** @brief queued() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of queued."}]] [[nodiscard]] auto op_queued() const -> access_op;

  /** @brief A predicate true once available() reaches @p count. */
  [[= py::doc{"Predicate: available() reaches count."}]] [[nodiscard]] auto
      predicate_available(std::uint64_t count) const -> native_predicate;

  /** @brief A predicate true while available() differs from @p mark. */
  [[= py::doc{"Predicate: available() differs from the mark."}]] [[nodiscard]] auto
      predicate_changed(count_mark const& mark) const -> native_predicate;

  /**
   * @brief send() as a command of MachineHandle.submit_ops(), every frame tagged with @p error.
   * @param data Frames, one per byte.
   * @param error A periph::uart_error value, 0 for clean frames.
   * @throws config_error when @p error is no uart_error, before anything is posted.
   */
  [[= py::doc{
      "Command of send, every byte tagged with the UartError value error."}]] [[nodiscard]] auto
      command_send(reflex::py::nb::bytes const& data, std::uint8_t error) const -> access_op;

  /** @brief A command of MachineHandle.submit_ops() queueing one break frame. */
  [[= py::doc{"Command queueing one break frame towards the firmware."}]] [[nodiscard]] auto
      command_break() const -> access_op;

  /**
   * @brief A command of MachineHandle.submit_ops() scheduling a change of the UART's nCTS
   *        input, see periph::uart_port::set_cts.
   * @param ready Whether the host is ready to receive.
   * @param at_ps Virtual time of the change in picoseconds, empty for the time the command runs.
   */
  [[= py::doc{"Command scheduling the UART's CTS input at at_ps, True for clear to send."}]]
      [[nodiscard]] auto command_set_cts(bool ready, std::optional<std::uint64_t> at_ps) const
      -> access_op;

  /**
   * @brief Whether the UART's nCTS input is ready with no change pending, see
   *        uart_port::cts_ready and uart_port::pending_cts.
   * @throws machine_running while the machine runs, outside its handlers.
   */
  [[= py::doc{
      "Whether the UART's CTS input is clear to send with no change pending."}]] [[nodiscard]] auto
      cts_settled_ready() const -> bool;

  /** @brief The UART's RTS output as ready to receive, see uart_port::ready_to_receive. */
  [[= py::doc{"Whether the UART's RTS output says ready to receive."}]] [[nodiscard]] auto
      rts_ready() const -> bool;

  /** @brief Frames transmitted by the firmware and not drained yet. */
  [[= py::doc{"Frames transmitted by the firmware and not drained yet."}]] [[nodiscard]] auto
      available() const -> std::uint64_t;

  /**
   * @brief Copies the low byte of queued frames without taking them, under the link lock.
   * @param start Queued frames skipped.
   * @param count Most bytes copied.
   */
  [[= py::doc{
      "The low bytes of at most count queued frames from start, left queued."}]] [[nodiscard]] auto
      peek(std::uint64_t start, std::uint64_t count) const -> reflex::py::nb::bytes;

  /**
   * @brief Takes the oldest transmitted frames.
   * @param count Most frames taken.
   */
  [[= py::doc{"Take at most count of the oldest transmitted frames."}]] auto
      drain(std::uint64_t count) -> std::vector<uart_frame>;

  /**
   * @brief Takes the oldest transmitted frames whose stop bit ended by @p until_ps, in one
   *        hold of the link lock: later frames stay queued.
   * @param count Most frames taken.
   * @param until_ps Latest stop bit end taken, in picoseconds since reset.
   */
  [[= py::doc{"Take at most count of the oldest frames whose at_ps is at most until_ps."}]] auto
      drain_until(std::uint64_t count, std::uint64_t until_ps) -> std::vector<uart_frame>;

  /** @brief Transmitted frames dropped because the queue held its limit. */
  [[= py::doc{"Transmitted frames dropped because the queue was full."}]] [[nodiscard]] auto
      dropped() const -> std::uint64_t;

  /**
   * @brief Sets the most transmitted frames the queue keeps, uart_frame_limit by default.
   * @param limit Frames, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most transmitted frames the queue keeps, for tests."}]] void
      set_frame_limit(std::uint64_t limit);

  /**
   * @brief The UART as a pty bridge's link, over the same queue.
   * @throws config_error once the machine is destroyed.
   */
  [[= py::skip]] [[nodiscard]] auto bridged_link() const -> std::unique_ptr<detail::pty_link>;

  /**
   * @brief The UART, for a linked system that attaches a wire to it.
   * @throws config_error once the machine is destroyed.
   */
  [[= py::skip]] [[nodiscard]] auto native_port() const -> periph::uart_port&;

  /** @brief The queue endpoint attached to the UART, which a wire forwards to. */
  [[= py::skip]] [[nodiscard]] auto endpoint() const noexcept
      -> std::shared_ptr<detail::queue_endpoint> const&;

private:
  [[nodiscard]] auto port() const -> periph::uart_port&;

  periph::uart_port*                      port_;
  std::shared_ptr<detail::queue_endpoint> endpoint_;
  detail::alive_flag                      alive_;
  detail::host_ref                        host_;
};
/** @brief One frame an SAI audio block transmitted, built when Python reads it. */
struct[[= py::rename{"AudioRecord"}]] audio_record
{
  /** @brief One word per slot, data right-aligned. */
  [[= py::readonly]] std::vector<std::uint32_t> slots{};
  /** @brief Data size in bits. */
  [[= py::readonly]] std::uint8_t data_bits{};
  /** @brief Active slots, bit n for slot n. */
  [[= py::readonly]] std::uint16_t active_slots{};
  /** @brief Mono mode with two slots. */
  [[= py::readonly]] bool mono{};
  /** @brief Virtual time the frame started, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
  /** @brief Bit clocks per frame, FRL + 1. */
  [[= py::readonly]] std::uint16_t frame_bits{};
  /** @brief Bits per slot, SLOTSZ resolved. */
  [[= py::readonly]] std::uint8_t slot_bits{};
  /** @brief First data bit within a slot, FBOFF. */
  [[= py::readonly]] std::uint8_t first_bit_offset{};
  /** @brief Frame sync marks the start of frame and the channel side, FSDEF. */
  [[= py::readonly]] bool fs_definition{};
  /** @brief Frame sync asserted one bit clock before slot 0, FSOFF. */
  [[= py::readonly]] bool fs_offset{};
  /** @brief Frame sync active high, FSPOL. */
  [[= py::readonly]] bool fs_active_high{};
  /** @brief Frame sync active level length in bit clocks, FSALL + 1. */
  [[= py::readonly]] std::uint8_t fs_active_bits{};
  /** @brief Start of each slot in bit clocks from the frame sync edge, NBSLOT + 1 of them. */
  [[= py::readonly]] std::vector<std::uint16_t> slot_starts{};
};

/**
 * @brief An SAI audio block of the machine connected to Python: frames the block transmits are
 *        kept with their start time, words Python writes are what the block receives. Valid
 *        while the machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"AudioLink"}]] audio_link
{
public:
  /**
   * @brief Connects a queue to a block, replacing the board's attachment.
   * @param port The SAI.
   * @param block Audio block.
   * @param decimation Frame time multiplier, 0 or 1 for the configured rate.
   * @param external_frame_hz Frame rate of an external clock master, 0 for none.
   * @param clock_wired A bus device drives the external clock at run time.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, through which the clock methods reach the SAI.
   */
  [[= py::skip]] audio_link(
      periph::stm32::audio_port& port,
      periph::stm32::audio_block block,
      std::uint32_t              decimation,
      std::uint32_t              external_frame_hz,
      bool                       clock_wired,
      detail::alive_flag         alive,
      detail::host_ref           host);

  /** @brief Frames transmitted and not drained yet. */
  [[= py::doc{"Frames transmitted and not drained yet."}]] [[nodiscard]] auto available() const
      -> std::uint64_t;
  /** @brief Transmitted frames dropped because the queue held its limit. */
  [[= py::doc{"Transmitted frames dropped because the queue was full."}]] [[nodiscard]] auto
      dropped() const -> std::uint64_t;
  /** @brief Words written and not served to the receiver yet. */
  [[= py::doc{"Words written and not served to the receiver yet."}]] [[nodiscard]] auto
      queued() const -> std::uint64_t;
  /**
   * @brief Layout of the frame the receiving block took last, with no slot words and the
   *        frame's start, none before the first frame or since the last discard.
   */
  [[= py::doc{
      "Layout and start of the frame the receiver took last, None before any."}]] [[nodiscard]] auto
      receive_layout() const -> std::optional<audio_record>;
  /**
   * @brief Takes the oldest transmitted frames.
   * @param count Most frames taken.
   */
  [[= py::doc{"Take at most count of the oldest transmitted frames."}]] auto
      drain(std::uint64_t count) -> std::vector<audio_record>;
  /**
   * @brief Takes the oldest transmitted frames that ended by @p until_ps, in one hold of the
   *        link lock: later frames stay queued.
   * @param count Most frames taken.
   * @param until_ps Latest frame end taken, in picoseconds since reset.
   */
  [[= py::doc{"Take at most count of the oldest frames that ended by until_ps."}]] auto
      drain_until(std::uint64_t count, std::uint64_t until_ps) -> std::vector<audio_record>;
  /**
   * @brief Queues words for a receiving block, one per slot in slot order.
   * @param words Words, inactive slots included.
   */
  [[= py::doc{"Queue words for a receiving block, one per slot in slot order."}]] void
      write(std::vector<std::uint32_t> const& words);
  /**
   * @brief Drops the kept frames, the words not served yet and the receiver's last layout, and
   *        clears the drop count.
   */
  [[= py::doc{
      "Drop kept frames, unserved words and the receive layout, clear the drop count."}]] void
      discard();
  /**
   * @brief Sets the most transmitted frames the queue keeps, audio_frame_limit by default.
   * @param limit Frames, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most transmitted frames the queue keeps, for tests."}]] void
      set_frame_limit(std::uint64_t limit);
  /**
   * @brief Starts, changes or stops the external frame clock of the block, as the board drives
   *        it, periph::stm32::audio_port::set_external_clock().
   * @param frame_hz Frames per second, 0 to stop.
   */
  [[= py::doc{"Start, change or stop the block's external frame clock, 0 to stop."}]] void
      set_external_frame_hz(std::uint32_t frame_hz);
  /** @brief The external frame rate set last, 0 when stopped. */
  [[= py::doc{"The block's external frame rate, 0 when stopped."}]] [[nodiscard]] auto
      external_frame_hz() const -> std::uint32_t;

  /** @brief external_frame_hz() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of external_frame_hz."}]] [[nodiscard]] auto op_external_frame_hz() const
      -> access_op;

  /** @brief write() as a command of MachineHandle.submit_ops(). */
  [[= py::doc{"Command of write."}]] [[nodiscard]] auto
      command_write(std::vector<std::uint32_t> const& words) const -> access_op;

  /** @brief A predicate true once available() reaches @p count. */
  [[= py::doc{"Predicate: available() reaches count."}]] [[nodiscard]] auto
      predicate_available(std::uint64_t count) const -> native_predicate;

  /** @brief set_external_frame_hz() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of set_external_frame_hz."}]] [[nodiscard]] auto
      op_set_external_frame_hz(std::uint32_t frame_hz) const -> access_op;

private:
  [[nodiscard]] auto endpoint() const -> detail::audio_queue_endpoint&;
  [[nodiscard]] auto port() const -> periph::stm32::audio_port&;

  std::shared_ptr<detail::audio_queue_endpoint> endpoint_;
  periph::stm32::audio_port*                    port_;
  periph::stm32::audio_block                    block_;
  detail::alive_flag                            alive_;
  detail::host_ref                              host_;
};
}
