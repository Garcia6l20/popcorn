#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view spi_ll = "l476_l4_spi_ll";
constexpr std::uint32_t    txe    = 1u << 1;

class loopback final : public periph::spi_target
{
public:
  std::vector<std::uint32_t> outs;

  void select(bool, vtime) override
  {}

  auto exchange(std::uint32_t out, std::uint8_t, vtime) -> std::uint32_t override
  {
    outs.push_back(out);
    return out;
  }

  auto exchange_deferred(std::uint32_t out, std::uint8_t bits, vtime at, std::uint64_t)
      -> std::optional<std::uint32_t> override
  {
    return exchange(out, bits, at);
  }

  void attached(periph::spi_target_port&) override
  {}

  void detached() noexcept override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}
};

struct run
{
  std::shared_ptr<loopback>  device = std::make_shared<loopback>();
  std::vector<std::uint32_t> logged;

  run(std::string_view entry, part_description const& desc)
  {
    stm32::board board{};
    board.spi["SPI1"].push_back({.device = device});
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, desc);
    auto f         = stm32_fixture{desc, spi_ll, std::move(options)};
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(spi_ll, "main")}).reason
        == stop_reason::address);
    f.set("pc", sym(spi_ll, entry));
    auto const stopped = f.run_to_stop({.max_instructions = icount{1'000'000}});
    CAPTURE(stopped.detail);
    REQUIRE(stopped.reason == stop_reason::user);
    logged = f.log();
  }
};
}

TEST_CASE("class coverage: the L476 LL and HAL SPI runs give the same result on L471, L475, L486")
{
  auto const digits = std::vector<std::uint32_t>{0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37};
  auto       dma    = std::vector<std::uint32_t>{0, 0, 0, 0, 0x6727, 0};
  dma.insert(dma.end(), digits.begin(), digits.end());
  dma.insert(dma.end(), {0, txe});
  for(auto const* variable : {"POPCORN_L471RG_DESC", "POPCORN_L475RG_DESC", "POPCORN_L486RG_DESC"})
  {
    CAPTURE(variable);
    auto const desc = popcorn::testing::description_from(variable);
    {
      run const r{"master8", desc};
      CHECK(r.logged == std::vector<std::uint32_t>{0x11, 0x22, 0x33, 0x44, txe});
      CHECK(r.device->outs == std::vector<std::uint32_t>{0x11, 0x22, 0x33, 0x44});
    }
    {
      run const r{"hal_txrx_dma_odd", desc};
      CHECK(r.logged == dma);
      CHECK(r.device->outs == digits);
    }
  }
}
