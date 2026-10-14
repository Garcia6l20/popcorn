#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/uart.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace popcorn::periph::stm32
{
/** @brief Most CTS changes a UART keeps pending, uart_port::set_cts. */
inline constexpr std::size_t max_pending_cts = 65536;

/**
 * @brief The side of the UART model a link to another machine drives, a uart_wire or the port's
 *        own loopback_endpoint. Reach it with `dynamic_cast<uart_link_port*>(machine.model(name))`.
 */
class POPCORN_API uart_link_port : public uart_port
{
public:
  /**
   * @brief Queues a frame from another machine. Its data reaches the receiver at @p frame.end,
   *        exactly, even when that time has already passed, in key order with the events of
   *        that instant, see core::scheduler. Its word keeps its sender's parity bit, or is the
   *        word as sampled when its shape differs, and is checked against the receiver's
   *        parity: a mismatch sets PE. Its start bit cancels an IDLE due after it. Called on this
   *        machine's host thread while no slice runs, or from its own loopback endpoint. A break
   *        frame reads as 0 with FE and never sets PE. A frame's FE, NE and PE rise when it
   *        reaches RDR, as a host frame's. Host frames of receive_frames() and these are not
   *        arbitrated: a linked UART takes no host frames.
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

protected:
  /** @brief Destroys the interface, only through its model. */
  ~uart_link_port() override;
};

/**
 * @brief Feeds every transmitted frame back into a port's receiver: data frames through
 *        uart_port::receive, a break through uart_link_port::receive_at as a break frame, lane 1.
 *        So a data frame reaches the receiver one receiver frame time after its stop-bit end, as
 *        any host frame, while a break reaches it at its own end: a data frame and the break
 *        right after it land at the same count, the data frame first by lane.
 */
class POPCORN_API loopback_endpoint final : public uart_endpoint
{
public:
  /**
   * @brief Loops back into @p port.
   * @param port Port whose receiver gets the frames, which must outlive the endpoint's use.
   */
  explicit loopback_endpoint(uart_link_port& port) : port_{&port}
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

  /** @brief Destroys the endpoint. Declared out-of-line: the key function pins the vtable. */
  ~loopback_endpoint() override;

private:
  uart_link_port* port_;
};
}
