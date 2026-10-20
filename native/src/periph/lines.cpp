#include "line_bus.hpp"

#include <popcorn/periph/core_hold.hpp>
#include <popcorn/periph/lines.hpp>
#include <popcorn/periph/reset_listener.hpp>

namespace popcorn::periph
{
core_hold_listener::~core_hold_listener() = default;
reset_listener::~reset_listener()         = default;

irq_line::irq_line(line_bus& bus, std::uint32_t source) noexcept : bus_{&bus}, source_{source}
{}

void irq_line::raise()
{
  if(not level_)
  {
    level_ = true;
    if(bus_ != nullptr)
    {
      bus_->set_irq(source_, true);
    }
  }
}

void irq_line::lower()
{
  if(level_)
  {
    level_ = false;
    if(bus_ != nullptr)
    {
      bus_->set_irq(source_, false);
    }
  }
}

auto irq_line::level() const -> bool
{
  return level_;
}

dma_request::dma_request(line_bus& bus, std::uint32_t source) noexcept : bus_{&bus}, source_{source}
{}

void dma_request::request()
{
  if(not level_)
  {
    level_ = true;
    if(bus_ != nullptr)
    {
      bus_->set_dma(source_, true);
    }
  }
}

void dma_request::release()
{
  if(level_)
  {
    level_ = false;
    if(bus_ != nullptr)
    {
      bus_->set_dma(source_, false);
    }
  }
}

auto dma_request::level() const -> bool
{
  return level_;
}
}
