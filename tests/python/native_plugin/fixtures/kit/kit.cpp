#include "kit.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <cstring>
#include <memory>

namespace kit
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

line_model::line_model(popcorn::peripheral_desc const& desc, popcorn::core::clock const& clock)
    : name_{desc.name}, transmit_{popcorn::find_register(desc, "TDR")},
      receive_{popcorn::find_register(desc, "RDR")}, clock_{&clock}
{}

auto line_model::claims() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const* const reg : {transmit_, receive_})
  {
    if(reg != nullptr)
    {
      names.push_back(reg->name);
    }
  }
  return names;
}

void line_model::reset(popcorn::periph::register_file&, popcorn::reset_kind)
{
  received_.clear();
}

auto line_model::on_read(
    popcorn::periph::register_file&,
    std::uint32_t offset,
    std::uint8_t,
    popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error>
{
  if(receive_ == nullptr or offset != receive_->offset or received_.empty())
  {
    return rdr_empty;
  }
  auto const word = received_.front();
  received_.pop_front();
  return word;
}

auto line_model::on_write(
    popcorn::periph::register_file&,
    std::uint32_t offset,
    std::uint8_t,
    std::uint32_t,
    std::uint32_t value,
    popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error>
{
  if(transmit_ != nullptr and offset == transmit_->offset and value == unmodelled_frame)
  {
    throw popcorn::unmodelled_config{name_, transmit_->name, offset,
                                     "TDR", value,           "a frame the kit refuses"};
  }
  if(transmit_ != nullptr and offset == transmit_->offset and far_ != nullptr)
  {
    far_->transmitted(static_cast<std::uint16_t>(value & frame_mask), clock_->time(), false);
  }
  return {};
}

void line_model::on_event(std::uint32_t, std::uint64_t)
{}

void line_model::save(std::vector<std::byte>&) const
{}

void line_model::load(std::span<std::byte const>)
{}

void line_model::receive_frames(std::span<popcorn::periph::host_frame const> frames)
{
  for(auto const& frame : frames)
  {
    received_.push_back(frame.data);
  }
}

auto line_model::queued() const -> std::size_t
{
  return received_.size();
}

void line_model::set_cts(bool ready, popcorn::vtime)
{
  cts_ = ready;
}

auto line_model::pending_cts() const -> std::size_t
{
  return 0;
}

auto line_model::cts_ready() const -> bool
{
  return cts_;
}

auto line_model::ready_to_receive() const -> bool
{
  return true;
}

void line_model::attach(std::shared_ptr<popcorn::periph::uart_endpoint> far_end)
{
  far_ = std::move(far_end);
}

void target::select(bool, popcorn::vtime)
{}

auto target::exchange(std::uint32_t out, std::uint8_t, popcorn::vtime) -> std::uint32_t
{
  return out + 3;
}

void target::save(std::vector<std::byte>&) const
{}

void target::load(std::span<std::byte const>)
{}
}

POPCORN_PLUGIN(kit, r)
{
  r.add_model("rng", [](popcorn::periph::model_context&, popcorn::peripheral_desc const& desc) {
    return std::make_shared<kit::counter_model>(desc);
  });
  r.add_model(
      "uart", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const& desc) {
        return std::make_shared<kit::line_model>(desc, ctx.clock);
      });
}
