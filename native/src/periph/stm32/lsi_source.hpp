#pragma once

namespace popcorn::periph::stm32
{
/**
 * @brief The RCC model as the IWDG model sees it, found by `dynamic_cast` over the machine's
 *        peers. RM0456 section 11.4.16: starting the independent watchdog forces the LSI on.
 */
class lsi_source
{
public:
  /** @brief Creates the interface. */
  lsi_source() = default;
  /** @brief Not copyable. */
  lsi_source(lsi_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(lsi_source const&) -> lsi_source& = delete;
  /** @brief Not movable. */
  lsi_source(lsi_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(lsi_source&&) -> lsi_source& = delete;

  /**
   * @brief Keeps the LSI on until the next reset, whatever `RCC_BDCR.LSION` says, starting it
   *        when it is off. LSIRDY follows after the board's ready delay, and the `lsi` domain
   *        with it.
   */
  virtual void force_lsi() = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~lsi_source() = default;
};
}
