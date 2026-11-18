#pragma once

namespace popcorn::periph::stm32
{
/**
 * @brief The SBS model as the FLASH model sees it, found by `dynamic_cast` over the machine's
 *        peers. RM0481 7.9.10 and 14.5.19: SBS_ECCNMIR.ECCNMI_MASK_EN stops the NMI of a double
 *        ECC error on a data access to the OTP.
 */
class ecc_nmi_mask_source
{
public:
  /** @brief Creates the interface. */
  ecc_nmi_mask_source() = default;
  /** @brief Not copyable. */
  ecc_nmi_mask_source(ecc_nmi_mask_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(ecc_nmi_mask_source const&) -> ecc_nmi_mask_source& = delete;
  /** @brief Not movable. */
  ecc_nmi_mask_source(ecc_nmi_mask_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(ecc_nmi_mask_source&&) -> ecc_nmi_mask_source& = delete;

  /** @brief Whether SBS_ECCNMIR.ECCNMI_MASK_EN is set. */
  [[nodiscard]] virtual auto ecc_nmi_masked() const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~ecc_nmi_mask_source() = default;
};
}
