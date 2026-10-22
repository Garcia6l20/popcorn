#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/bus_reset.hpp>
#include <popcorn/periph/model.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace native_plugin
{
/** @brief Virtual time of one conversion, as `rngmodel.CONVERSION` of the python_model example. */
inline constexpr std::uint64_t conversion_ps = 40'000'000;

/**
 * @brief The STM32U575 RNG of the python_model example's `rngmodel.Rng`, in C++: a seeded
 *        xorshift32 sequence on DR, DRDY one conversion after RNGEN, a DR read or the write
 *        clearing CONDRST, CONDRST cancelling the conversion, the interrupt line following
 *        DRDY while IE is set. RCC_AHB2RSTR1.RNGRST resets it as a machine reset does.
 */
class rng final : public popcorn::periph::model, public popcorn::periph::bus_reset_listener
{
public:
  /**
   * @brief Serves the RNG described by @p desc.
   * @param ctx What the machine hands the model, kept for the model's life.
   * @param desc The RNG peripheral.
   * @param seed First state of the sequence, 1 to 2^32 - 1.
   * @throws popcorn::config_error when @p desc has no CR, SR or DR register.
   */
  rng(popcorn::periph::model_context& ctx,
      popcorn::peripheral_desc const& desc,
      std::uint32_t                   seed);

  [[nodiscard]] auto claims() const -> std::vector<std::string> override;
  void               reset(popcorn::periph::register_file& regs, popcorn::reset_kind kind) override;
  void               on_bus_reset() override;
  auto               on_read(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      popcorn::periph::access_ctx ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override;
  auto on_write(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      std::uint32_t                   old,
      std::uint32_t                   value,
      popcorn::periph::access_ctx     ctx) -> std::expected<void, popcorn::bus_error> override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

private:
  struct state
  {
    std::uint32_t word;
    std::uint64_t pending;
  };

  void start();
  void stop();
  void follow();

  popcorn::periph::model_context* ctx_;
  popcorn::register_desc const*   cr_;
  popcorn::register_desc const*   sr_;
  popcorn::register_desc const*   dr_;
  std::uint32_t                   rngen_;
  std::uint32_t                   ie_;
  std::uint32_t                   condrst_;
  std::uint32_t                   drdy_;
  std::uint32_t                   seed_;
  state                           state_;
};
}
