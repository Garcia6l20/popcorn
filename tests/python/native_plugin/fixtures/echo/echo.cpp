#include "echo.hpp"

#include <popcorn/plugin/plugin.hpp>

#include <memory>

namespace echo
{
void echo_target::select(bool, popcorn::vtime)
{}

auto echo_target::exchange(std::uint32_t out, std::uint8_t, popcorn::vtime) -> std::uint32_t
{
  return out + 1;
}

void echo_target::save(std::vector<std::byte>&) const
{}

void echo_target::load(std::span<std::byte const>)
{}
}

POPCORN_PLUGIN(echo, r)
{
  r.add_model("rng", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
    return std::shared_ptr<popcorn::periph::model>{};
  });
}
