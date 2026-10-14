#include <popcorn/core/errors.hpp>
#include <popcorn/periph/devices/i2c_table.hpp>

#include <reflex/serde/bson.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <utility>
#include <vector>

namespace popcorn::periph::devices
{
namespace
{
struct table_state
{
  std::uint8_t              writing{};
  std::uint8_t              reading{};
  std::uint8_t              address_count{};
  std::uint32_t             reg{};
  std::uint32_t             position{};
  std::vector<std::uint8_t> data;
  std::vector<table_write>  writes;

  friend auto operator==(table_state const&, table_state const&) -> bool = default;
};

auto checked(table_target_options options) -> table_target_options
{
  if(options.address >= 0x80)
  {
    throw config_error(
        "table_target", 0, "address", std::format("a 7-bit address, not {:#x}", options.address));
  }
  if(options.register_bytes == 0 or options.register_bytes > 4)
  {
    throw config_error(
        "table_target", 0, "register_bytes", std::format("1 to 4, not {}", options.register_bytes));
  }
  return options;
}
}

struct table_target::impl
{
  table_target_options options;
  table_state          state{};

  explicit impl(table_target_options opts) : options{checked(std::move(opts))}
  {}

  void end_write()
  {
    auto const open      = state.writing != 0;
    auto const addressed = state.address_count == options.register_bytes;
    if(open and addressed and not state.data.empty())
    {
      state.writes.push_back({.reg = state.reg, .bytes = state.data});
    }
    state.writing = 0;
    state.reading = 0;
    state.data.clear();
  }
};

table_target::table_target(table_target_options options)
    : impl_{std::make_unique<impl>(std::move(options))}
{}

table_target::~table_target() = default;

auto table_target::address() const -> std::uint8_t
{
  return impl_->options.address;
}

auto table_target::start(bool read, vtime) -> i2c_reply
{
  auto& self = *impl_;
  self.end_write();
  if(read)
  {
    if(self.options.nack_unknown and not self.options.replies.contains(self.state.reg))
    {
      return {.ack = false};
    }
    self.state.reading  = 1;
    self.state.position = 0;
    return {.ack = true};
  }
  self.state.writing       = 1;
  self.state.address_count = 0;
  self.state.reg           = 0;
  return {.ack = true};
}

auto table_target::write(std::uint8_t byte, vtime) -> i2c_reply
{
  auto& self = *impl_;
  if(self.state.writing == 0)
  {
    return {.ack = false};
  }
  if(self.state.address_count < self.options.register_bytes)
  {
    self.state.reg = (self.state.reg << 8) | byte;
    ++self.state.address_count;
    return {.ack = true};
  }
  self.state.data.push_back(byte);
  return {.ack = true};
}

auto table_target::read(vtime) -> i2c_read_reply
{
  auto& self = *impl_;
  if(self.state.reading == 0)
  {
    return {.byte = 0xFF};
  }
  auto const found = self.options.replies.find(self.state.reg);
  if(found == self.options.replies.end() or self.state.position >= found->second.size())
  {
    return {.byte = 0xFF};
  }
  return {.byte = found->second[self.state.position++]};
}

void table_target::stop(vtime)
{
  impl_->end_write();
}

void table_target::reset(reset_kind kind)
{
  if(kind != reset_kind::power_on and kind != reset_kind::brown_out)
  {
    return;
  }
  auto writes         = std::move(impl_->state.writes);
  impl_->state        = {};
  impl_->state.writes = std::move(writes);
}

void table_target::save(std::vector<std::byte>& out) const
{
  reflex::serde::bson::serializer{out}.dump(impl_->state);
}

void table_target::load(std::span<std::byte const> in)
{
  impl_->state = reflex::serde::bson::deserializer{in}.template load<table_state>();
}

auto table_target::writes() const -> std::span<table_write const>
{
  return impl_->state.writes;
}
}
