#include "fixture.hpp"

#include <popcorn/plugin/plugin.hpp>

#include <cstring>
#include <memory>

namespace fixture
{
counter_model::counter_model(popcorn::peripheral_desc const& desc)
    : data_{popcorn::find_register(desc, "DR")}
{}

auto counter_model::claims() const -> std::vector<std::string>
{
  if(data_ == nullptr)
  {
    return {};
  }
  return {data_->name};
}

void counter_model::reset(popcorn::periph::register_file&, popcorn::reset_kind)
{
  reads_ = 0;
}

auto counter_model::on_read(
    popcorn::periph::register_file& regs,
    std::uint32_t                   offset,
    std::uint8_t,
    popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error>
{
  auto const value = rng_base + reads_++;
  regs.poke(offset & ~3u, value);
  return value;
}

auto counter_model::on_write(
    popcorn::periph::register_file&,
    std::uint32_t,
    std::uint8_t,
    std::uint32_t,
    std::uint32_t,
    popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error>
{
  return {};
}

void counter_model::on_event(std::uint32_t, std::uint64_t)
{}

void counter_model::save(std::vector<std::byte>& out) const
{
  auto const at = out.size();
  out.resize(at + sizeof reads_);
  std::memcpy(out.data() + at, &reads_, sizeof reads_);
}

void counter_model::load(std::span<std::byte const> in)
{
  if(in.size() == sizeof reads_)
  {
    std::memcpy(&reads_, in.data(), sizeof reads_);
  }
}

void target::select(bool, popcorn::vtime)
{}

auto target::exchange(std::uint32_t out, std::uint8_t, popcorn::vtime) -> std::uint32_t
{
  return out + 1;
}

void target::save(std::vector<std::byte>&) const
{}

void target::load(std::span<std::byte const>)
{}
}

POPCORN_PLUGIN(fixture_plugin, r)
{
  r.add_model("rng", [](popcorn::periph::model_context&, popcorn::peripheral_desc const& desc) {
    return std::make_shared<fixture::counter_model>(desc);
  });
}
