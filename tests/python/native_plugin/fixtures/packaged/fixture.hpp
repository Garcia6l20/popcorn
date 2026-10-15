#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fixture
{
inline constexpr std::uint32_t rng_base = 0xF1C5'0000;

class counter_model : public popcorn::periph::model
{
public:
  explicit counter_model(popcorn::peripheral_desc const& desc);

  [[nodiscard]] auto claims() const -> std::vector<std::string> override;
  void               reset(popcorn::periph::register_file& regs, popcorn::reset_kind kind) override;
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
  popcorn::register_desc const* data_;
  std::uint32_t                 reads_{};
};

class target : public popcorn::periph::spi_target
{
public:
  void select(bool selected, popcorn::vtime at) override;
  auto exchange(std::uint32_t out, std::uint8_t bits, popcorn::vtime at) -> std::uint32_t override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;
};
}
