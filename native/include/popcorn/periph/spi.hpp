#pragma once

/**
 * @file
 * @brief The interfaces of a device on an SPI bus and of a controller that drives one.
 */

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace popcorn::periph
{
class spi_target_port;

/**
 * @brief A device on an SPI bus, driven by the SPI model of the controller it is attached to.
 *        Full duplex: every frame out returns a frame in. Frames cross in wire order,
 *        right-aligned, the first bit on the wire in bit `bits - 1`. Times are the machine's
 *        virtual time since power-on, which a system reset does not restart.
 */
class POPCORN_API spi_target
{
public:
  /** @brief Creates the device. */
  spi_target() = default;
  /** @brief Not copyable. */
  spi_target(spi_target const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(spi_target const&) -> spi_target& = delete;
  /** @brief Not movable. */
  spi_target(spi_target&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(spi_target&&) -> spi_target& = delete;
  /** @brief Destroys the device. */
  virtual ~spi_target();

  /**
   * @brief The chip select changed.
   * @param selected Whether the device is now selected, chip select low.
   * @param at Virtual time since power-on of the edge.
   */
  virtual void select(bool selected, vtime at) = 0;

  /**
   * @brief Exchanges one frame while the device is selected.
   * @param out Frame the controller sends, in wire order, right-aligned.
   * @param bits Frame size, 1 to 32, the size the firmware configured.
   * @param at Virtual time since power-on at the end of the frame.
   * @return Frame the device returns, in wire order, right-aligned, bits above @p bits ignored.
   */
  [[nodiscard]] virtual auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t = 0;

  /**
   * @brief Exchanges one frame, with a reply the device may give later. The SPI model calls
   *        only this form, at the end of the frame's shift. A reply given here completes the
   *        frame at once, as exchange() does. An empty result leaves the frame pending: the
   *        device completes it through spi_target_port::complete() with @p ticket. The frame's
   *        end, its RX FIFO entry, its flags and the next frame then follow the latest
   *        completion of every selected device, and SCK stays idle until then. The default
   *        returns exchange().
   * @param out Frame the controller sends, in wire order, right-aligned.
   * @param bits Frame size, 1 to 32, the size the firmware configured.
   * @param at Virtual time since power-on at the end of the frame's shift.
   * @param ticket Identifies this frame to spi_target_port::complete().
   * @return Frame the device returns, as for exchange(), or empty while the reply is pending.
   */
  [[nodiscard]] virtual auto
      exchange_deferred(std::uint32_t out, std::uint8_t bits, vtime at, std::uint64_t ticket)
          -> std::optional<std::uint32_t>
  {
    static_cast<void>(ticket);
    return exchange(out, bits, at);
  }

  /**
   * @brief The device was attached to the SPI model of an instance, at the model's
   *        construction for a board device. The port stays valid until detached(). The default
   *        does nothing.
   * @param port The model the device completes deferred frames through.
   */
  virtual void attached([[maybe_unused]] spi_target_port& port)
  {}

  /**
   * @brief The device was detached from the port attached() gave, when the model is destroyed
   *        for a board device. The device keeps no reference to that port after the call. The
   *        default does nothing.
   */
  virtual void detached() noexcept
  {}

  /**
   * @brief The SPI mode the device works in, CPOL << 1 | CPHA. The SPI model reads it once per
   *        transfer and device, at the first frame exchanged, and fails the run with
   *        config_error when the firmware clocks the bus in another mode. The default declares
   *        none, which is never checked.
   * @return 0 to 3, or empty for no check.
   */
  [[nodiscard]] virtual auto mode() const -> std::optional<std::uint8_t>
  {
    return std::nullopt;
  }

  /**
   * @brief The machine was reset. The SPI model calls it for every attached device, in
   *        attachment order, after its own reset. The default does nothing.
   * @param kind Cause of the reset. A device on the board takes power_on and brown_out as its
   *        own supply events and any other kind as an MCU reset it does not see. After power_on
   *        the times passed to later calls restart from 0, so on power_on and brown_out a device
   *        clears every time it keeps along with its volatile state.
   */
  virtual void reset([[maybe_unused]] reset_kind kind)
  {}

  /**
   * @brief The supply was cut. The SPI model calls it for every attached device, in
   *        attachment order, from its own power_cut(), before the power-on reset of
   *        `core::machine::power_cycle()`. The default does nothing. A device holding a
   *        non-volatile array applies what the cut leaves of an operation in flight. When a
   *        device throws, every later device is still cut, and the first exception is rethrown
   *        after.
   * @param at Virtual time since power-on of the cut.
   */
  virtual void power_cut([[maybe_unused]] vtime at)
  {}

  /**
   * @brief Appends the device state to a snapshot.
   * @param out Snapshot bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores the device state from bytes save() wrote, so that every later call behaves
   *        as it would have after the save.
   * @param in Bytes of one save() call.
   */
  virtual void load(std::span<std::byte const> in) = 0;
};

/**
 * @brief What the SPI model of an instance offers the spi_target devices attached to it, to
 *        complete the frames they left pending in spi_target::exchange_deferred().
 */
class POPCORN_API spi_target_port
{
public:
  /**
   * @brief Completes a pending frame at @p at with @p word. The call only schedules the
   *        completion. When it falls due, a completion whose @p ticket is not the frame still
   *        pending, after a reset, after SPE was cleared or for a frame already complete, is
   *        refused and counted in the model's saved state, never applied. Each completion is
   *        one event on @p lane, so it fires after the model's own events due at the same
   *        instant, in the order of @p lane.
   * @param ticket The ticket exchange_deferred() received.
   * @param word Frame the device returns, in wire order, right-aligned.
   * @param at Virtual time since power-on of the completion. A due time in the past keeps its
   *        own instruction count, as a post a link hands at a drain may have.
   * @param lane Delivery lane of the completion, 1 and up.
   * @throws std::invalid_argument When @p at is before the last reset, or @p lane is 0.
   */
  virtual void complete(std::uint64_t ticket, std::uint32_t word, vtime at, std::uint32_t lane) = 0;

protected:
  /** @brief Destroyed through its model only. */
  virtual ~spi_target_port();
};

/**
 * @brief A controller on an SPI bus, outside the MCU, clocking an SPI in target mode. It drives
 *        frames through the instance's spi_controller_port and hears each frame back here. One
 *        per SPI instance, none next to spi_target devices. Frames cross in wire order, as for
 *        spi_target. Times are the machine's time since its last reset.
 */
class POPCORN_API spi_controller_sink
{
public:
  /** @brief Creates the controller. */
  spi_controller_sink() = default;
  /** @brief Not copyable. */
  spi_controller_sink(spi_controller_sink const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(spi_controller_sink const&) -> spi_controller_sink& = delete;
  /** @brief Not movable. */
  spi_controller_sink(spi_controller_sink&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(spi_controller_sink&&) -> spi_controller_sink& = delete;
  /** @brief Destroys the controller. */
  virtual ~spi_controller_sink();

  /**
   * @brief One frame of a transfer ended.
   * @param sent Frame the controller sent, in wire order, right-aligned.
   * @param received Frame the controller received from the MCU, in wire order, all ones when
   *        the SPI did not drive MISO, right-aligned.
   * @param at Machine time since reset at the end of the frame.
   */
  virtual void frame(std::uint32_t sent, std::uint32_t received, vtime at) = 0;

  /**
   * @brief A frame or unit the SPI model will never clock, reported once, on the machine thread,
   *        from the event of the lane the unit was queued on. A reset of the target, a deselect
   *        due before the frame's end or spi_controller_port::drop() drops a unit. The default
   *        does nothing.
   * @param unit The unit the frame was queued with, 0 for a frame of
   *        spi_controller_port::transfer().
   * @param sent All ones masked to the frame size, all ones for a unit of
   *        spi_controller_port::drop().
   * @param at Machine time since reset at which the unit is reported: the frame's end, or the
   *        reset's time when that is later, or the deselect's time, or the time drop() gave.
   */
  virtual void dropped(
      [[maybe_unused]] std::uint64_t unit,
      [[maybe_unused]] std::uint32_t sent,
      [[maybe_unused]] vtime         at)
  {}

  /**
   * @brief The SPI mode the controller clocks in, CPOL << 1 | CPHA. The SPI model reads it once
   *        per transfer, at the first frame the target takes part in, and fails the run with
   *        config_error when the firmware configured another mode. The default declares none,
   *        which is never checked.
   * @return 0 to 3, or empty for no check.
   */
  [[nodiscard]] virtual auto mode() const -> std::optional<std::uint8_t>
  {
    return std::nullopt;
  }
};

/**
 * @brief What the SPI model of an instance offers the spi_controller_sink attached to it.
 */
class POPCORN_API spi_controller_port
{
public:
  /** @brief Most frames queued at once over every transfer not finished. */
  static constexpr std::size_t max_frames = 4096;

  /**
   * @brief Queues one transfer: the chip select input selected at its start, @p frames shifted
   *        one frame time apart, chip select released at the end of the last one. A transfer
   *        starts at the later of @p start and the end of the transfers queued before it.
   * @param start Machine time since reset of the earliest start, not before the current time.
   * @param frame_ps Length of one frame, at least 1.
   * @param bits Frame size, 4 to 32.
   * @param frames The frames in wire order, 1 to max_frames, each below 2 to the @p bits.
   * @return Machine time since reset at which the transfer starts.
   * @throws config_error When no controller is attached to the instance, or the queue would
   *         hold more than max_frames frames.
   * @throws std::invalid_argument When @p start is past, the transfer ends past the time
   *         range, an argument is out of its range, or a unit primitive other than a dropped
   *         unit is queued or holds the chip select.
   */
  virtual auto transfer(
      vtime                          start,
      std::uint64_t                  frame_ps,
      std::uint8_t                   bits,
      std::span<std::uint32_t const> frames) -> vtime = 0;

  /**
   * @brief Queues the chip select input falling at @p at, on @p lane. The level stays until a
   *        deselect() and survives every reset of the target. The unit primitives, select(),
   *        frame(), deselect() and drop(), each carry a @p unit strictly above the one before.
   *        The port runs their entries due at one instruction count in unit order, after the
   *        events of lower lanes, whatever order they were queued in. A due time in the past
   *        keeps its own instruction count.
   * @param at Machine time since reset.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error When no controller is attached to the instance, or the port would hold
   *         more than max_frames entries.
   * @throws std::invalid_argument When @p lane is 0, @p unit is not above the last unit, or a
   *         transfer() is queued.
   */
  virtual void select(vtime at, std::uint32_t lane, std::uint64_t unit) = 0;

  /**
   * @brief Queues one frame on @p lane, shifted from the latest of @p at, the end of every frame
   *        queued before it and the time of every queued deselect() due after the last RCC reset
   *        of the instance. A frame of a lower unit than a queued deselect() does not count, since
   *        that deselect() cuts it or follows its end. The controller hears the frame through
   *        spi_controller_sink::frame() at its end, or through spi_controller_sink::dropped(). A
   *        frame whose start is at or before the last RCC reset of the instance is dropped,
   *        reported at the later of its end and that reset.
   * @param word Frame the controller sends, in wire order, below 2 to the @p bits.
   * @param bits Frame size, 4 to 32.
   * @param frame_ps Length of the frame, at least 1.
   * @param at Machine time since reset of the earliest start.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error As select().
   * @throws std::invalid_argument As select(), or when an argument is out of its range or the
   *         frame ends past the time range.
   */
  virtual void frame(
      std::uint32_t word,
      std::uint8_t  bits,
      std::uint64_t frame_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit) = 0;

  /**
   * @brief Queues the chip select input rising at the later of @p at and the last RCC reset of
   *        the instance, on @p lane. Every frame of a lower unit still queued or shifting then is
   *        dropped and reported at that time. A unit already dropped keeps its own report time.
   * @param at Machine time since reset.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error As select().
   * @throws std::invalid_argument As select().
   */
  virtual void deselect(vtime at, std::uint32_t lane, std::uint64_t unit) = 0;

  /**
   * @brief Queues a unit the caller knows is lost, reported through
   *        spi_controller_sink::dropped() at the later of @p at and the last RCC reset of the
   *        instance.
   * @param at Machine time since reset.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error As select().
   * @throws std::invalid_argument As select().
   */
  virtual void drop(vtime at, std::uint32_t lane, std::uint64_t unit) = 0;

protected:
  /** @brief Destroyed through its model only. */
  virtual ~spi_controller_port();
};
}
