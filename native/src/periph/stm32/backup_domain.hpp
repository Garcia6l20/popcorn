#pragma once

namespace popcorn::periph::stm32
{
/**
 * @brief A model with backup-domain state, the RTC and TAMP models, found by `dynamic_cast` over
 *        the machine's peers. The RCC model calls it on a backup domain reset, RM0456 section
 *        63.3.13: RCC_BDCR.BDRST set.
 */
class backup_domain
{
public:
  /** @brief Creates the interface. */
  backup_domain() = default;
  /** @brief Not copyable. */
  backup_domain(backup_domain const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(backup_domain const&) -> backup_domain& = delete;
  /** @brief Not movable. */
  backup_domain(backup_domain&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(backup_domain&&) -> backup_domain& = delete;

  /** @brief Brings the backup-domain registers and state to their reset values. */
  virtual void backup_reset() = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~backup_domain() = default;
};
}
