#pragma once

#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief Parity of a UART frame. */
enum class[[= reflex::derive{reflex::Format}]] line_parity
{
  /** @brief No parity bit. */
  none,
};

/** @brief What a UART frame carries. */
enum class[[= reflex::derive{reflex::Format}]] frame_kind
{
  /** @brief A data frame. */
  data,
  /** @brief A break: the line held low for a frame, data and stop bits 0, then 2 bits high. */
  brk,
};

/** @brief One frame on a UART line, as its sender shaped it. */
struct line_frame
{
  /** @brief Data bits, right-aligned. */
  std::uint16_t data{};
  /** @brief M bits: 7, 8 or 9, parity included when there is one. */
  std::uint8_t word_bits{8};
  /** @brief Stop length in half bits: 1, 2, 3 or 4. */
  std::uint8_t half_stops{2};
  /** @brief Parity. */
  line_parity parity{};
  /** @brief Data or not. */
  frame_kind kind{};
  /** @brief Sender frame time, start bit to stop-bit end, in picoseconds. */
  std::uint64_t frame_ps{};
  /** @brief Start-bit edge. */
  vtime start{};
  /** @brief Stop-bit end. */
  vtime end{};

  /** @brief Member-wise equality. */
  friend auto operator==(line_frame const&, line_frame const&) -> bool = default;
};

/**
 * @brief The far end of a UART line. Receives what the firmware transmits, one frame at a time.
 *        Called from the UART model's events and register writes, on the machine's host thread in
 *        the middle of a slice: it must not run the machine, takes no lock but a leaf, and never
 *        feeds another machine. It may feed the port's own receiver.
 */
class uart_endpoint
{
public:
  /** @brief Creates the endpoint. */
  uart_endpoint() = default;
  /** @brief Not copyable. */
  uart_endpoint(uart_endpoint const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(uart_endpoint const&) -> uart_endpoint& = delete;
  /** @brief Not movable. */
  uart_endpoint(uart_endpoint&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(uart_endpoint&&) -> uart_endpoint& = delete;
  /** @brief Destroys the endpoint. */
  virtual ~uart_endpoint() = default;

  /**
   * @brief A frame left the transmitter.
   * @param data Data bits of the frame, 7 to 9 of them, right-aligned, 0 for a break.
   * @param at Virtual time the stop bit ended, the break's last low bit for a break.
   * @param brk Whether the frame is a break.
   */
  virtual void transmitted(std::uint16_t data, vtime at, bool brk) = 0;

  /**
   * @brief A frame's start bit left the transmitter: its whole shape and its times are known.
   *        The default does nothing.
   * @param frame The frame, times in the sender's virtual time.
   */
  virtual void started([[maybe_unused]] line_frame const& frame)
  {}

  /**
   * @brief The transmitter was disabled, UE cleared, with a frame in flight: that frame will not
   *        complete. Not called on a reset. The default does nothing.
   * @param at Virtual time of the clear, in the sender's time.
   */
  virtual void aborted([[maybe_unused]] vtime at)
  {}
};

/**
 * @brief The firmware side of a UART as a board sees it, implemented by the UART model. Reach it
 *        with `dynamic_cast<uart_port*>(machine.model("USART1"))`.
 */
class uart_port
{
public:
  /** @brief Creates the interface. */
  uart_port() = default;
  /** @brief Not copyable. */
  uart_port(uart_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(uart_port const&) -> uart_port& = delete;
  /** @brief Not movable. */
  uart_port(uart_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(uart_port&&) -> uart_port& = delete;

  /**
   * @brief Queues frames on the line towards the firmware. They arrive one frame time apart at the
   *        configured baud, the first one frame time from now. The queue is saved in snapshots.
   *        An MCU reset drops the frames still queued, as the line keeps no data on hardware.
   * @param data Frames, data bits right-aligned.
   */
  virtual void receive(std::span<std::uint16_t const> data) = 0;

  /** @brief Host frames still queued by receive(), not yet delivered to the receiver. */
  [[nodiscard]] virtual auto queued() const -> std::size_t = 0;

  /**
   * @brief Queues a frame from another machine. Its data reaches the receiver at @p frame.end,
   *        exactly, even when that time has already passed, in key order with the events of
   *        that instant, see core::scheduler. Its start bit cancels an IDLE due after it. Called
   *        on this machine's host thread while no slice runs, or from its own loopback endpoint.
   *        A break frame reads as 0 with FE. Host frames of receive() and these
   *        are not arbitrated: a linked UART takes no host frames.
   * @param frame The frame, times already in this machine's virtual time.
   * @param lane Lane of the wire it came on, 1 and up.
   */
  virtual void receive_at(line_frame const& frame, std::uint32_t lane) = 0;

  /**
   * @brief Drops the queued frame of @p lane in flight at @p at, start before and end after, the
   *        sender having aborted it. The line goes idle at @p at. Called as receive_at().
   * @param at Virtual time of the abort, in this machine's time.
   * @param lane Lane of the wire.
   */
  virtual void cancel_in_flight(vtime at, std::uint32_t lane) = 0;

  /**
   * @brief Connects the far end, replacing the board's endpoint. Not saved in snapshots.
   * @param far_end Endpoint, empty to disconnect: transmitted frames are then dropped.
   */
  virtual void attach(std::shared_ptr<uart_endpoint> far_end) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~uart_port() = default;
};

/** @brief One frame a capture_endpoint kept. */
struct captured_frame
{
  /** @brief Data bits. */
  std::uint16_t data{};
  /** @brief Virtual time its stop bit ended. */
  vtime at{};
  /** @brief Whether it was a break. */
  bool brk{};

  /** @brief Member-wise equality. */
  friend auto operator==(captured_frame const&, captured_frame const&) -> bool = default;
};

/** @brief Keeps every transmitted frame with its time. */
class capture_endpoint final : public uart_endpoint
{
public:
  /** @brief Frames received, in order. */
  std::vector<captured_frame> frames;

  void transmitted(std::uint16_t data, vtime at, bool brk) override
  {
    frames.push_back({.data = data, .at = at, .brk = brk});
  }
};

/**
 * @brief Feeds every transmitted frame back into a port's receiver: data frames through
 *        uart_port::receive, a break through uart_port::receive_at as a break frame, lane 1. So a
 *        data frame reaches the receiver one receiver frame time after its stop-bit end, as any
 *        host frame, while a break reaches it at its own end: a data frame and the break right
 *        after it land at the same count, the data frame first by lane.
 */
class loopback_endpoint final : public uart_endpoint
{
public:
  /**
   * @brief Loops back into @p port.
   * @param port Port whose receiver gets the frames, which must outlive the endpoint's use.
   */
  explicit loopback_endpoint(uart_port& port) : port_{&port}
  {}

  void transmitted(std::uint16_t data, vtime, bool brk) override
  {
    if(not brk)
    {
      port_->receive(std::span{&data, 1});
    }
  }

  void started(line_frame const& frame) override
  {
    if(frame.kind == frame_kind::brk)
    {
      port_->receive_at(frame, 1);
    }
  }

private:
  uart_port* port_;
};
}
