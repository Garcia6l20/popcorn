#include <popcorn/periph/register_file.hpp>

namespace popcorn::periph
{
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
}
