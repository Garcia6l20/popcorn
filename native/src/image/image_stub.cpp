#include <popcorn/image/image.hpp>
#include <popcorn/image/raw.hpp>

#include <popcorn/core/errors.hpp>

namespace popcorn::image
{
auto image::lookup(std::string_view) const -> symbol const&
{
  throw not_implemented("image::lookup");
}

auto image::locate(address) const -> location
{
  throw not_implemented("image::locate");
}

auto load_elf(std::filesystem::path const&) -> image
{
  throw not_implemented("load_elf");
}

auto load_bin(std::filesystem::path const&, address) -> image
{
  throw not_implemented("load_bin");
}

auto load_hex(std::filesystem::path const&) -> image
{
  throw not_implemented("load_hex");
}
}
