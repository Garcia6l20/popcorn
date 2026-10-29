#pragma once

#include <cstdint>
#include <optional>

namespace popcorn::periph::stm32
{
/**
 * @brief The classic EXTI model as its line sources see it, found by `dynamic_cast` over the
 *        machine's peers: the input level of one line, edges detected on the change.
 */
class exti_line_input
{
public:
  /** @brief Creates the interface. */
  exti_line_input() = default;
  /** @brief Not copyable. */
  exti_line_input(exti_line_input const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(exti_line_input const&) -> exti_line_input& = delete;
  /** @brief Not movable. */
  exti_line_input(exti_line_input&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(exti_line_input&&) -> exti_line_input& = delete;

  /**
   * @brief The input of a line changed level. A line the EXTI lacks is ignored.
   * @param line Line number.
   * @param level New level.
   */
  virtual void line_changed(std::uint8_t line, bool level) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~exti_line_input() = default;
};

/**
 * @brief The SYSCFG model as the classic EXTI sees it, found by `dynamic_cast` over the machine's
 *        peers: the GPIO port each of the lines 0 to 15 takes, from `SYSCFG_EXTICRn`.
 */
class exti_selector
{
public:
  /** @brief Creates the interface. */
  exti_selector() = default;
  /** @brief Not copyable. */
  exti_selector(exti_selector const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(exti_selector const&) -> exti_selector& = delete;
  /** @brief Not movable. */
  exti_selector(exti_selector&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(exti_selector&&) -> exti_selector& = delete;

  /**
   * @brief Port line @p line takes its pin from.
   * @param line Line number, 0 to 15.
   * @return The port by letter, 0 for GPIOA, std::nullopt when the selection names no port of
   *         the family for that line.
   */
  [[nodiscard]] virtual auto selected_port(std::uint8_t line) const
      -> std::optional<std::uint8_t> = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~exti_selector() = default;
};

/**
 * @brief The SYSCFG model as the USB device model sees it, found by `dynamic_cast` over the
 *        machine's peers: the internal D+ pull-up enable, `SYSCFG_PMC.USB_PU` on the STM32L1.
 */
class usb_pull_up_source
{
public:
  /** @brief Creates the interface. */
  usb_pull_up_source() = default;
  /** @brief Not copyable. */
  usb_pull_up_source(usb_pull_up_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(usb_pull_up_source const&) -> usb_pull_up_source& = delete;
  /** @brief Not movable. */
  usb_pull_up_source(usb_pull_up_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(usb_pull_up_source&&) -> usb_pull_up_source& = delete;

  /** @brief Whether the pull-up enable is set. */
  [[nodiscard]] virtual auto usb_pull_up() const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~usb_pull_up_source() = default;
};
}
