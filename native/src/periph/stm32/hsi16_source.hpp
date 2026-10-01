#pragma once

namespace popcorn::periph::stm32
{
/**
 * @brief The RCC model as the FLASH model sees it, found by `dynamic_cast` over the machine's
 *        peers. RM0456 sections 7.3.6 and 7.3.7: HSI16 runs while FLASH_NSCR.PG or STRT is set,
 *        then stops unless RCC_CR.HSION keeps it on.
 */
class hsi16_source
{
public:
  /** @brief Creates the interface. */
  hsi16_source() = default;
  /** @brief Not copyable. */
  hsi16_source(hsi16_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(hsi16_source const&) -> hsi16_source& = delete;
  /** @brief Not movable. */
  hsi16_source(hsi16_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(hsi16_source&&) -> hsi16_source& = delete;

  /**
   * @brief Keeps HSI16 on while @p held, starting it when it is off, HSIRDY following after
   *        the board's ready delay. Releasing it stops HSI16 unless RCC_CR.HSION is set or the
   *        system clock uses it. A reset releases it.
   * @param held Whether the FLASH interface needs HSI16.
   */
  virtual void hold_hsi16(bool held) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~hsi16_source() = default;
};
}
