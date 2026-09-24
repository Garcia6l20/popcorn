#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/lines.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include <string>
#include <utility>

namespace popcorn::periph
{
void irq_line::raise()
{
  throw not_implemented("irq_line::raise");
}

void irq_line::lower()
{
  throw not_implemented("irq_line::lower");
}

auto irq_line::level() const -> bool
{
  return false;
}

void dma_request::request()
{
  throw not_implemented("dma_request::request");
}

void dma_request::release()
{
  throw not_implemented("dma_request::release");
}

auto dma_request::level() const -> bool
{
  return false;
}

register_file::register_file(peripheral_desc const& desc)
    : desc_{&desc}, words_((desc.size + 3) / 4)
{}

register_file::~register_file() = default;

auto register_file::description() const noexcept -> peripheral_desc const&
{
  return *desc_;
}

auto register_file::peek(std::uint32_t offset) const -> std::uint32_t
{
  return words_[offset / 4];
}

void register_file::poke(std::uint32_t offset, std::uint32_t value)
{
  words_[offset / 4] = value;
}

void model_context::request_stop(stop_reason reason, std::string detail)
{
  services.request_stop(reason, std::move(detail));
}

void register_model(std::string_view, std::string_view, model_factory)
{
  throw not_implemented("register_model");
}
}
