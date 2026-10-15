#include "adc.hpp"

#include "saved.hpp"

namespace native_plugin
{
namespace
{
constexpr std::uint32_t start       = 0x8000;
constexpr std::uint32_t channel_bit = 12;
constexpr std::uint8_t  frame_bits  = 16;
constexpr std::uint32_t full_scale  = 0xFFF;
constexpr std::uint64_t ps_per_ms   = 1'000'000'000;
constexpr std::size_t   saved_size  = 5 * sizeof(std::uint32_t);
}

adc::adc(std::uint32_t counts_per_ms) : counts_per_ms_{counts_per_ms}
{}

void adc::select(bool selected, popcorn::vtime)
{
  if(selected)
  {
    ++state_.counts.selects;
    state_.frame = 0;
    return;
  }
  ++state_.counts.deselects;
  if(state_.frame != 2)
  {
    ++state_.counts.bad_frames;
  }
}

auto adc::exchange(std::uint32_t out, std::uint8_t bits, popcorn::vtime at) -> std::uint32_t
{
  ++state_.frame;
  if(bits != frame_bits)
  {
    ++state_.counts.bad_frames;
  }
  if(state_.frame == 1)
  {
    if((out & start) != 0)
    {
      state_.channel = (out >> channel_bit) & 1;
    }
    return 0;
  }
  if(state_.frame == 2)
  {
    auto const ramp   = static_cast<unsigned __int128>(at.ps) * counts_per_ms_ / ps_per_ms;
    auto const rising = static_cast<std::uint32_t>(ramp) & full_scale;
    return state_.channel == 0 ? rising : full_scale - rising;
  }
  return 0;
}

void adc::reset(popcorn::reset_kind kind)
{
  if(kind == popcorn::reset_kind::power_on or kind == popcorn::reset_kind::brown_out)
  {
    state_.frame   = 0;
    state_.channel = 0;
  }
}

void adc::save(std::vector<std::byte>& out) const
{
  append(out, state_.counts.selects);
  append(out, state_.counts.deselects);
  append(out, state_.counts.bad_frames);
  append(out, state_.frame);
  append(out, state_.channel);
}

void adc::load(std::span<std::byte const> in)
{
  expect_size("adc", in, saved_size);
  state loaded{};
  loaded.counts.selects    = take<std::uint32_t>(in);
  loaded.counts.deselects  = take<std::uint32_t>(in);
  loaded.counts.bad_frames = take<std::uint32_t>(in);
  loaded.frame             = take<std::uint32_t>(in);
  loaded.channel           = take<std::uint32_t>(in);
  state_                   = loaded;
}

auto adc::counted() const noexcept -> framing
{
  return state_.counts;
}

auto adc::counts_per_ms() const noexcept -> std::uint32_t
{
  return counts_per_ms_;
}
}
