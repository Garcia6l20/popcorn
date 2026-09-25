#pragma once

#include <popcorn/periph/model.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::testing
{
/**
 * @brief Native model of the core test description's `TESTDEV`. Claims `DATA` and `CTRL`,
 *        stores writes as they come, and on a write of 1 to `CTRL.GO` raises its IRQ line after
 *        delay instructions through its event port. A read of `DATA` lowers the line.
 *        Test-only.
 */
class test_model final : public periph::model
{
public:
  /** @brief Offset of `DATA` in `TESTDEV`. */
  static constexpr std::uint32_t data_offset = 0x58;
  /** @brief Offset of `CTRL` in `TESTDEV`. */
  static constexpr std::uint32_t ctrl_offset = 0x5c;
  /** @brief Instructions between a `CTRL.GO` write and the IRQ. */
  static constexpr std::uint64_t delay = 100;

  /**
   * @brief Creates the model over its context, which it keeps.
   * @param ctx Context of `TESTDEV`.
   */
  explicit test_model(periph::model_context& ctx) noexcept : context_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override;
  void               reset(periph::register_file& regs, reset_kind kind) override;
  auto               on_read(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      periph::access_ctx     ctx) -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      std::uint32_t          old,
      std::uint32_t          value,
      periph::access_ctx     ctx) -> std::expected<void, bus_error> override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  /** @brief Context the model was created with. */
  [[nodiscard]] auto context() const noexcept -> periph::model_context&;

  /** @brief Whether reset() raises the IRQ line, for the reset order test. */
  bool raise_in_reset = false;
  /** @brief Kind of the last reset, empty before the first. */
  std::optional<reset_kind> last_reset{};

private:
  periph::model_context& context_;
};

/** @brief Factory creating a test_model. */
auto test_model_factory() -> periph::model_factory;
}
