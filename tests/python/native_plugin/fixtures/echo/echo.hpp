#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/periph/spi.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace echo
{
class echo_target : public popcorn::periph::spi_target
{
public:
  void select(bool selected, popcorn::vtime at) override;
  auto exchange(std::uint32_t out, std::uint8_t bits, popcorn::vtime at) -> std::uint32_t override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;
};
}
