#pragma once

#include <popcorn/core/clock.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/i2c.hpp>

#include <memory>

namespace popcorn::periph::stm32
{
class i2c_link_port;

/**
 * @brief The controller-side proxy of an I2C link: a target a link attaches to an I2C instance at
 *        run time, through i2c_link_port::attach_target(). The I2C model treats it as a board
 *        target attached after every board target, with three differences. It hears every data
 *        phase of the controller's transfers, whatever it answered to the address, and answers a
 *        data phase of an address it did not acknowledge with NACK or 0xFF and no stretch. It
 *        hears the STOP and the lost arbitration of every transfer. Its acknowledged flag lives in
 *        its link, out of the model's saved acknowledged set: the model calls matches() at every
 *        address phase, a repeated START included, and the flag follows the last one, so a target
 *        clears it in matches() and sets it from the answer to that address.
 */
class POPCORN_API i2c_link_target : public i2c_target
{
public:
  /**
   * @brief The target is being attached to @p port, before i2c_target::attached(). Both
   *        references stay valid until i2c_target::detached(), after which the target keeps
   *        neither.
   * @param port The link port of the instance, which holds the target's mute flag.
   * @param clock Clock of the machine, read on the machine thread for the power-on offset
   *        `since_power_on() - time()`.
   */
  virtual void linked(i2c_link_port& port, core::clock const& clock) = 0;

  /** @brief Destroys the target. */
  ~i2c_link_target() override;
};

/**
 * @brief The target-side proxy of an I2C link: the controller a link attaches to an I2C instance
 *        at run time, through i2c_link_port::attach_controller(). It drives the instance as a
 *        board controller does.
 */
class POPCORN_API i2c_link_controller : public i2c_controller_sink
{
public:
  /**
   * @brief The controller is attached to @p port. Both references stay valid until unlinked().
   * @param port The port the controller queues its phases on.
   * @param clock Clock of the machine.
   */
  virtual void linked(i2c_controller_port& port, core::clock const& clock) = 0;

  /** @brief The controller was detached. It keeps no reference linked() gave after the call. */
  virtual void unlinked() noexcept = 0;

  /** @brief Destroys the controller. */
  ~i2c_link_controller() override;
};

/**
 * @brief The side of the STM32 I2C model an I2C link attaches its proxies to, at run time. Reach
 *        it with `dynamic_cast<i2c_link_port*>(machine.model(name))`. Every call runs on the
 *        machine's host thread while no slice runs. The model's saved state holds the board
 *        attachments only: a member snapshot has the same bytes with and without the link
 *        attachments and their mute flags while no link target defers an answer. A deferred answer
 *        is saved as a held phase and a completion not yet due, and a refused completion moves the
 *        saved refused count.
 */
class POPCORN_API i2c_link_port
{
public:
  /**
   * @brief Attaches a controller-side proxy after every board target and every proxy attached
   *        before it, unmuted. Calls i2c_link_target::linked(), then i2c_target::attached(), with
   *        the proxy already attached. When linked() or attached() throws, the proxy is not
   *        attached and hears no i2c_target::detached(), so a proxy whose attached() throws undoes
   *        its own linked() before the throw. A proxy attached during a transfer hears the later
   *        phases of that transfer, its data phases as for an address it did not acknowledge, and
   *        its STOP.
   * @param target The proxy.
   * @throws config_error When a controller is attached to the instance.
   * @throws std::invalid_argument When @p target is empty or already attached.
   */
  virtual void attach_target(std::shared_ptr<i2c_link_target> target) = 0;

  /**
   * @brief Calls i2c_target::detached() on a controller-side proxy, still attached during the
   *        call, then detaches it. The model keeps no reference to it after the call. A phase the
   *        proxy left pending in a deferred call stays held until PE is cleared or a reset, so a
   *        proxy owing an answer completes it through i2c_target_port::complete() or
   *        i2c_target_port::complete_read() before it is detached. A completion it made before the
   *        detach lands when it falls due. Does nothing for a proxy not attached.
   * @param target The proxy.
   */
  virtual void detach_target(i2c_link_target& target) noexcept = 0;

  /**
   * @brief Attaches the target-side proxy as the instance's controller and calls
   *        i2c_link_controller::linked(). Clears the port's last unit, so its units start again
   *        from 1. When linked() throws, the proxy is not attached and the last unit is kept.
   * @param controller The proxy.
   * @throws config_error When the instance has a board controller or board targets, or a
   *         controller-side proxy is attached.
   * @throws std::invalid_argument When @p controller is empty or a target-side proxy is attached.
   */
  virtual void attach_controller(std::shared_ptr<i2c_link_controller> controller) = 0;

  /**
   * @brief Detaches the target-side proxy and calls i2c_link_controller::unlinked(). Removes the
   *        port's queued entries without reporting them and clears its last unit. A transfer the
   *        proxy left open ends there: the bus is free, BUSY clears and the I2C waits for its next
   *        address match, with no STOPF. Does nothing for a proxy not attached.
   * @param controller The proxy.
   */
  virtual void detach_controller(i2c_link_controller& controller) noexcept = 0;

  /**
   * @brief Sets the mute flag of a controller-side proxy. The proxy reads it through muted() and
   *        posts nothing while it is set, whatever the model calls on it.
   * @param target An attached proxy.
   * @throws std::invalid_argument When @p target is not attached.
   */
  virtual void mute(i2c_link_target const& target) = 0;

  /**
   * @brief Clears the mute flag of a controller-side proxy.
   * @param target An attached proxy.
   * @throws std::invalid_argument When @p target is not attached.
   */
  virtual void unmute(i2c_link_target const& target) = 0;

  /**
   * @brief The mute flag of a controller-side proxy, readable from i2c_link_target::linked()
   *        until i2c_target::detached() returns.
   * @param target An attached proxy.
   * @throws std::invalid_argument When @p target is not attached.
   */
  [[nodiscard]] virtual auto muted(i2c_link_target const& target) const -> bool = 0;

protected:
  /** @brief Destroyed through its model only. */
  virtual ~i2c_link_port();
};
}
