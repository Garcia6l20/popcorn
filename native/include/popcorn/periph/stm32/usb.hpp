#pragma once

#include <popcorn/export.hpp>
#include <popcorn/periph/usb_host.hpp>

namespace popcorn::periph::stm32
{
/** @brief The virtual host a board wires to a USB device controller. */
struct usb_host_options
{
  /** @brief Whether the cable is plugged at power-on. Unplugged, the host sees no device until
   * usb_host_port::plug() plugs it. */
  bool attached{true};
  /** @brief Behaviour of the host: timings, class drivers and the warning sink. An empty
   * `warn` logs model warnings of the controller. */
  usb::host_options host{};
};

/**
 * @brief The virtual host of a USB device controller as a board sees it, implemented by the
 *        OTG_FS model. The model owns the host, schedules its wake-ups and keeps its state and
 *        the cable in its snapshots. Reach it with
 *        `dynamic_cast<usb_host_port*>(machine.model("OTG_FS"))`.
 */
class POPCORN_API usb_host_port
{
public:
  /** @brief Creates the interface. */
  usb_host_port() = default;
  /** @brief Not copyable. */
  usb_host_port(usb_host_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(usb_host_port const&) -> usb_host_port& = delete;
  /** @brief Not movable. */
  usb_host_port(usb_host_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(usb_host_port&&) -> usb_host_port& = delete;

  /** @brief The board's host, null when the board wires none to this controller. */
  [[nodiscard]] virtual auto board_host() -> usb::host* = 0;

  /** @brief Whether the cable to the board's host is plugged: VBUS powered, the host told of
   * the pull-up. */
  [[nodiscard]] virtual auto plugged() const -> bool = 0;

  /**
   * @brief Plugs or unplugs the cable to the board's host. Unplugging tells the host the device
   *        is gone, which drops its drivers and its transfers. A power-on reset restores
   *        usb_host_options::attached. Saved in snapshots. Without a board host, nothing
   *        happens.
   * @param attached Whether the cable is plugged.
   */
  virtual void plug(bool attached) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~usb_host_port();
};
}
