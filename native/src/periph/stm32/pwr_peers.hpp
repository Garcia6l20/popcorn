#pragma once

namespace popcorn::periph::stm32
{
/**
 * @brief The PWR model as the OTG_FS model sees it, found by `dynamic_cast` over the machine's
 *        peers: whether the USB supply VDDUSB is declared valid, RM0351 5.4.2 and RM0456 10.10.5.
 */
class usb_supply_source
{
public:
  /** @brief Creates the interface. */
  usb_supply_source() = default;
  /** @brief Not copyable. */
  usb_supply_source(usb_supply_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(usb_supply_source const&) -> usb_supply_source& = delete;
  /** @brief Not movable. */
  usb_supply_source(usb_supply_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(usb_supply_source&&) -> usb_supply_source& = delete;

  /** @brief `PWR_CR2.USV` on the STM32L4, `PWR_SVMCR.USV` on the STM32U5, true on a family
   *         without it. */
  [[nodiscard]] virtual auto usb_supply_valid() const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~usb_supply_source() = default;
};

/**
 * @brief The OTG_FS model as the PWR model sees it, found by `dynamic_cast` over the machine's
 *        peers: told of every change of usb_supply_source::usb_supply_valid.
 */
class usb_supply_listener
{
public:
  /** @brief Creates the interface. */
  usb_supply_listener() = default;
  /** @brief Not copyable. */
  usb_supply_listener(usb_supply_listener const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(usb_supply_listener const&) -> usb_supply_listener& = delete;
  /** @brief Not movable. */
  usb_supply_listener(usb_supply_listener&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(usb_supply_listener&&) -> usb_supply_listener& = delete;

  /** @brief The USB supply changed, read it again through usb_supply_source. */
  virtual void usb_supply_changed() = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~usb_supply_listener() = default;
};
}
