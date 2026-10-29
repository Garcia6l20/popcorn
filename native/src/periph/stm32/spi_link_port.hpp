#pragma once

#include <popcorn/core/clock.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/spi.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace popcorn::periph::stm32
{
class spi_link_port;

/**
 * @brief The master-side proxy of an SPI link: a device a link attaches to an SPI instance at run
 *        time, through spi_link_port::attach_target(). The SPI model treats it as a board device
 *        attached after every board device, with three differences: it is selected at each frame
 *        end by the level of its chip select pin or by the SS output, the model runs no mode
 *        check on it, and its link saves its state.
 */
class POPCORN_API spi_link_target : public spi_target
{
public:
  /**
   * @brief The device is being attached to @p port, before spi_target::attached(). Both
   *        references stay valid until spi_target::detached(), after which the device keeps
   *        neither.
   * @param port The link port of the instance, which holds the device's mute flag.
   * @param clock Clock of the machine, read on the machine thread for the power-on offset
   *        `since_power_on() - time()`.
   */
  virtual void linked(spi_link_port& port, core::clock const& clock) = 0;

  /** @brief Destroys the device. */
  ~spi_link_target() override;
};

/**
 * @brief The slave-side proxy of an SPI link: the controller a link attaches to an SPI instance
 *        at run time, through spi_link_port::attach_controller(). It drives the instance as a
 *        board controller does.
 */
class POPCORN_API spi_link_controller : public spi_controller_sink
{
public:
  /**
   * @brief The controller is attached to @p port. Both references stay valid until unlinked().
   * @param port The port the controller queues its units on.
   * @param clock Clock of the machine.
   */
  virtual void linked(spi_controller_port& port, core::clock const& clock) = 0;

  /** @brief The controller was detached. It keeps no reference linked() gave after the call. */
  virtual void unlinked() noexcept = 0;

  /**
   * @brief A frame queued through spi_controller_port::frame() ended. The model calls it in
   *        place of spi_controller_sink::frame() for every such frame, with the unit the frame
   *        was queued with. The default calls spi_controller_sink::frame().
   * @param unit Unit number of the frame.
   * @param sent The frame the controller sent.
   * @param received The frame the instance returned.
   * @param at Time of the frame end since the last reset.
   */
  virtual void unit_frame(std::uint64_t unit, std::uint32_t sent, std::uint32_t received, vtime at);

  /** @brief Destroys the controller. */
  ~spi_link_controller() override;
};

/**
 * @brief The side of the STM32 SPI model an SPI link attaches its proxies to, at run time. Reach
 *        it with `dynamic_cast<spi_link_port*>(machine.model(name))`. Every call runs on the
 *        machine's host thread while no slice runs. The model's saved state holds the board
 *        attachments only: a member snapshot has the same bytes with and without the link
 *        attachments and their mute flags.
 */
class POPCORN_API spi_link_port
{
public:
  /**
   * @brief Attaches a master-side proxy after every board device and every proxy attached
   *        before it, unmuted. Calls spi_link_target::linked(), then spi_target::attached(),
   *        with the proxy already attached, then spi_target::select() with true when its chip
   *        select is active at the call. A proxy with a chip select pin hears that pin's edges
   *        through spi_target::select(). When linked() or attached() throws, the proxy is not
   *        attached and hears no spi_target::detached(), so a proxy whose attached() throws
   *        undoes its own linked() before the throw.
   * @param target The proxy.
   * @param cs_port GPIO port of the chip select, such as `GPIOA`, ignored without @p cs_pin.
   * @param cs_pin Chip select pin, active low, or empty for the SS output: selected from CSTART
   *        to the end of the transfer while CFG2.SSOE is set.
   * @throws config_error When a controller is attached to the instance, or @p cs_port names no
   *         GPIO port.
   * @throws std::invalid_argument When @p target is empty or already attached, or @p cs_pin is
   *         not a pin of the port.
   */
  virtual void attach_target(
      std::shared_ptr<spi_link_target> target,
      std::string                      cs_port,
      std::optional<std::uint8_t>      cs_pin) = 0;

  /**
   * @brief Calls spi_target::detached() on a master-side proxy, still attached during the call,
   *        then detaches it. The model keeps no reference to it after the call. A frame the
   *        proxy left pending in spi_target::exchange_deferred() stays pending until SPE is
   *        cleared, so a proxy owing a reply completes it through spi_target_port::complete()
   *        before it is detached. Does nothing for a proxy not attached.
   * @param target The proxy.
   */
  virtual void detach_target(spi_link_target& target) noexcept = 0;

  /**
   * @brief Attaches the slave-side proxy as the instance's controller and calls
   *        spi_link_controller::linked(). Clears the port's last unit and chip select level, so
   *        its units start again from 1.
   * @param controller The proxy.
   * @throws config_error When the instance has a board controller or board devices, or a
   *         master-side proxy is attached.
   * @throws std::invalid_argument When @p controller is empty or a slave-side proxy is attached.
   */
  virtual void attach_controller(std::shared_ptr<spi_link_controller> controller) = 0;

  /**
   * @brief Detaches the slave-side proxy and calls spi_link_controller::unlinked(). Removes the
   *        port's queued entries without reporting them and clears its last unit and chip select
   *        level. Does nothing for a proxy not attached.
   * @param controller The proxy.
   */
  virtual void detach_controller(spi_link_controller& controller) noexcept = 0;

  /**
   * @brief Sets the mute flag of a master-side proxy. The proxy reads it through muted() and
   *        posts nothing while it is set, whatever the model calls on it.
   * @param target An attached proxy.
   * @throws std::invalid_argument When @p target is not attached.
   */
  virtual void mute(spi_link_target const& target) = 0;

  /**
   * @brief Clears the mute flag of a master-side proxy.
   * @param target An attached proxy.
   * @return The level of its chip select, true high, read as at a frame end: the GPIO level of
   *         its pin, or the SS output.
   * @throws std::invalid_argument When @p target is not attached.
   */
  [[nodiscard]] virtual auto unmute(spi_link_target const& target) -> bool = 0;

  /**
   * @brief The mute flag of a master-side proxy, readable from spi_link_target::linked() until
   *        spi_target::detached() returns.
   * @param target An attached proxy.
   * @throws std::invalid_argument When @p target is not attached.
   */
  [[nodiscard]] virtual auto muted(spi_link_target const& target) const -> bool = 0;

  /**
   * @brief Length of one frame at the instance's current kernel clock, prescaler and frame size.
   * @return Picoseconds, 0 while the kernel clock is stopped.
   */
  [[nodiscard]] virtual auto frame_time() const -> std::uint64_t = 0;

protected:
  /** @brief Destroyed through its model only. */
  virtual ~spi_link_port();
};
}
