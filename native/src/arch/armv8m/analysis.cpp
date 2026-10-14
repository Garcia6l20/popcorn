#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/arch/armv8m/thumb.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace popcorn::arch::armv8m
{
namespace
{
auto halfword(std::span<std::byte const> bytes, std::size_t at) noexcept -> std::uint16_t
{
  return static_cast<std::uint16_t>(
      std::to_integer<unsigned>(bytes[at]) | std::to_integer<unsigned>(bytes[at + 1]) << 8);
}

auto is_trap(thumb::insn const& i) noexcept -> bool
{
  return (i.size == 2 and (i.bits & 0xff00) == 0xde00)
      or (i.size == 4 and (i.bits & 0xfff0'f000) == 0xf7f0'a000);
}

class thumb_analysis final : public code_analysis
{
public:
  [[nodiscard]] auto size(std::span<std::byte const> first) const noexcept
      -> std::optional<std::uint8_t> override
  {
    if(first.size() < 2)
    {
      return std::nullopt;
    }
    return thumb::size(halfword(first, 0));
  }

  [[nodiscard]] auto decode(address at, std::span<std::byte const> bytes) const noexcept
      -> std::optional<decoded_insn> override
  {
    auto const read = thumb::read(at, bytes);
    if(not read)
    {
      return std::nullopt;
    }
    auto const control = thumb::classify(*read);
    return decoded_insn{
        .at          = read->at,
        .size        = read->size,
        .kind        = control.kind,
        .target      = control.target,
        .conditional = control.conditional,
        .call        = thumb::is_call(*read),
        .trap        = is_trap(*read),
        .predicates  = thumb::is_it(*read) ? thumb::it_length(*read) : std::uint8_t{0},
    };
  }

  [[nodiscard]] auto
      predicated_at_most(address at, address base, std::span<std::byte const> code) const noexcept
      -> std::uint8_t override
  {
    for(address back = 2; back <= 16; back += 2)
    {
      if(at < base + back)
      {
        break;
      }
      auto const offset = at - back - base;
      if(offset + 1 >= code.size())
      {
        continue;
      }
      if(thumb::is_it({.bits = halfword(code, offset), .size = 2}))
      {
        return 4;
      }
    }
    return 0;
  }

  [[nodiscard]] auto crosses_page(std::uint32_t offset, std::uint8_t width) const noexcept
      -> bool override
  {
    return offset >= page or (offset >= page - 3 and width == 4);
  }

  [[nodiscard]] auto limits() const noexcept -> block_limits override
  {
    return {.page = page, .max_insns = 512};
  }

private:
  static constexpr std::uint32_t page = 1024;
};

constinit thumb_analysis const instance{};
}

auto analysis() noexcept -> code_analysis const&
{
  return instance;
}
}
