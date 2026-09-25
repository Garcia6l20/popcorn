#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/image/image.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::consumers::image_loader
{
auto from_bin(std::filesystem::path const& file, address start, std::vector<std::byte> bytes)
    -> image::image
{
  image::image built;
  built.segments.push_back(
      {.name        = file.filename().string(),
       .start       = start,
       .bytes       = std::move(bytes),
       .memory_size = 0,
       .executable  = true,
       .writable    = false});
  built.segments.back().memory_size =
      static_cast<std::uint32_t>(built.segments.back().bytes.size());
  built.symbols.push_back(
      {.name  = "Reset_Handler",
       .value = start + 0x100,
       .size  = 8,
       .kind  = image::symbol_kind::function});
  built.files = {"startup.s"};
  built.lines = {
      {.addr = start + 0x100, .file = 0, .line = 12, .is_stmt = true },
      {.addr = start + 0x104, .file = 0, .line = 13, .is_stmt = false}
  };
  built.entry  = start + 0x101;
  built.abi    = arch::float_abi::hard_double;
  built.digest = std::string(64, '0');
  return built;
}

auto resolve(image::image const& loaded, std::string const& name) -> location
{
  auto const& found = loaded.lookup(name);
  return loaded.locate(found.value);
}
}

TEST_CASE("image loader consumer builds and resolves an image")
{
  auto const built = popcorn::consumers::image_loader::from_bin(
      "app.bin", 0x0800'0000, std::vector<std::byte>(16, std::byte{0xff}));
  CHECK(built.segments.front().memory_size == 16);
  CHECK(built.lines.size() == 2);
  auto const resolved = popcorn::consumers::image_loader::resolve(built, "Reset_Handler");
  CHECK(resolved.symbol == "Reset_Handler");
  CHECK(resolved.file == "startup.s");
  CHECK(resolved.line == 12);
  CHECK_THROWS_AS(popcorn::image::load_elf("app.elf"), popcorn::image_error);
}
