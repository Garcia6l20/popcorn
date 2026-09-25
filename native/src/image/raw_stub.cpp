#include <popcorn/image/raw.hpp>

#include <popcorn/core/errors.hpp>

namespace popcorn::image
{
auto load_bin(std::filesystem::path const&, address) -> image
{
  throw not_implemented("load_bin");
}

auto load_hex(std::filesystem::path const&) -> image
{
  throw not_implemented("load_hex");
}
}
