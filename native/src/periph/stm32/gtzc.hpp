#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief Illegal access event source of a TZIC: index n - 1 of its IERn, SRn and FCRn
 * registers and the source's bit within them. */
struct illegal_source
{
  /** @brief Register index, 0 for IER1, SR1 and FCR1. */
  std::uint8_t reg{};
  /** @brief Bit of the source. */
  std::uint32_t mask{};

  /** @brief Member-wise equality. */
  friend auto operator==(illegal_source const&, illegal_source const&) -> bool = default;
};

/**
 * @brief A GTZC TZIC as the other models see it, found by `dynamic_cast` over the machine's
 *        peers: records an illegal access event, RM0456 5.4.5. Only built under TrustZone.
 */
class illegal_access_controller
{
public:
  /** @brief Creates the interface. */
  illegal_access_controller() = default;
  /** @brief Not copyable. */
  illegal_access_controller(illegal_access_controller const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(illegal_access_controller const&) -> illegal_access_controller& = delete;
  /** @brief Not movable. */
  illegal_access_controller(illegal_access_controller&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(illegal_access_controller&&) -> illegal_access_controller& = delete;

  /**
   * @brief Source of an enable field of this TZIC.
   * @param enable_field IERn field name, such as `SRAM1IE`.
   * @return The source, empty when no IERn register of this TZIC has the field.
   */
  [[nodiscard]] virtual auto source(std::string_view enable_field) const
      -> std::optional<illegal_source> = 0;

  /**
   * @brief Source of a peripheral of the part in this TZIC.
   * @param unit Index of the peripheral in part_description::peripherals.
   * @return The source, empty when no IERn field of this TZIC names the peripheral.
   */
  [[nodiscard]] virtual auto source_of(std::size_t unit) const -> std::optional<illegal_source> = 0;

  /**
   * @brief Sets the source's SRn flag and drives GTZC_IRQn while an enabled flag is set. Never
   *        schedules an event, so a bus firewall may call it.
   * @param from Source, from source() or source_of() of this TZIC.
   */
  virtual void illegal_access(illegal_source from) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~illegal_access_controller() = default;
};

/**
 * @brief A GTZC MPCBB as the memory filters see it, found by `dynamic_cast` over the machine's
 *        peers: the security and privilege of each block of its SRAM, RM0456 5.4.4.
 */
class sram_block_controller
{
public:
  /** @brief Creates the interface. */
  sram_block_controller() = default;
  /** @brief Not copyable. */
  sram_block_controller(sram_block_controller const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(sram_block_controller const&) -> sram_block_controller& = delete;
  /** @brief Not movable. */
  sram_block_controller(sram_block_controller&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(sram_block_controller&&) -> sram_block_controller& = delete;

  /** @brief SRAM the controller covers, empty when the part's board data has no row for it. */
  [[nodiscard]] virtual auto blocks() const -> std::optional<sram_blocks> = 0;

  /**
   * @brief Whether the block holding an address is Secure, its SECCFGRx bit.
   * @param at Address in the Non-secure alias of the SRAM.
   * @return The bit, empty when the controller does not cover @p at.
   */
  [[nodiscard]] virtual auto block_secure(address at) const -> std::optional<bool> = 0;

  /**
   * @brief Whether the block holding an address is privileged, its PRIVCFGRx bit.
   * @param at Address in the Non-secure alias of the SRAM.
   * @return The bit, empty when the controller does not cover @p at.
   */
  [[nodiscard]] virtual auto block_privileged(address at) const -> std::optional<bool> = 0;

  /** @brief Whether CR.SRWILADIS lets Secure data accesses reach Non-secure blocks. */
  [[nodiscard]] virtual auto secure_data_allowed() const -> bool = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~sram_block_controller() = default;
};

/** @brief Policy of one flash page, RM0456 7.5.2 to 7.5.6. */
struct page_policy
{
  /** @brief Secure, by watermark or block bit. */
  bool secure{};
  /** @brief Privileged, by block bit. */
  bool privileged{};
  /** @brief In a hide-protection area whose access is disabled, denied to every access. */
  bool hidden{};

  /** @brief Member-wise equality. */
  friend auto operator==(page_policy const&, page_policy const&) -> bool = default;
};

/**
 * @brief A flash memory as the memory filter sees it, found by `dynamic_cast` over the
 *        machine's peers: the security, privilege and hide protection of each of its pages,
 *        RM0456 7.5.2 to 7.5.6.
 */
class secure_page_controller
{
public:
  /** @brief Creates the interface. */
  secure_page_controller() = default;
  /** @brief Not copyable. */
  secure_page_controller(secure_page_controller const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(secure_page_controller const&) -> secure_page_controller& = delete;
  /** @brief Not movable. */
  secure_page_controller(secure_page_controller&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(secure_page_controller&&) -> secure_page_controller& = delete;

  /**
   * @brief Whether the page holding an address is Secure.
   * @param at Address in any alias of the memory.
   * @return The page's security, empty when @p at is not in the memory or the memory has no
   *         page security, as without option bytes.
   */
  [[nodiscard]] virtual auto page_secure(address at) const -> std::optional<bool> = 0;

  /**
   * @brief Policy of the page holding an address.
   * @param at Address in any alias of the memory.
   * @return The policy, empty exactly when page_secure() is.
   */
  [[nodiscard]] virtual auto page_rule(address at) const -> std::optional<page_policy> = 0;

  /** @brief Size of the pages of page_secure() in bytes. */
  [[nodiscard]] virtual auto secure_page_size() const -> std::uint32_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~secure_page_controller() = default;
};

/** @brief What a TZSC SECCFGRx or TZIC IERx field names. */
enum class gtzc_target : std::uint8_t
{
  peripheral,
  memory,
  unknown,
};

/** @brief One SECCFGRx field of a TZSC or IERx field of a TZIC of the part. */
struct gtzc_field
{
  /** @brief TZSC or TZIC instance, such as `GTZC1_TZSC`. */
  std::string owner;
  /** @brief Register, such as `SECCFGR1`. */
  std::string reg;
  /** @brief Field, such as `TIM2SEC`. */
  std::string field;
  /** @brief What the field names. */
  gtzc_target target{};
  /** @brief Indexes in part_description::peripherals of the peripherals it names: one, or
   * two where RM0456 gives one bit to two blocks, such as `DCMISEC` for DCMI and PSSI. */
  std::vector<std::size_t> units;
};

/**
 * @brief Every SECCFGRx field of every TZSC and IERx field of every TZIC of a part, each
 *        resolved to the peripheral or memory it names. The models ignore an unknown field.
 * @param part Description.
 */
[[nodiscard]] auto gtzc_fields(part_description const& part) -> std::vector<gtzc_field>;
}
