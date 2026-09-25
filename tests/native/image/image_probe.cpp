#include <popcorn/image/image.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
auto run_bench(std::filesystem::path const& elf, int runs) -> int
{
  std::vector<double>   times_ms;
  popcorn::image::image img;
  for(int i = 0; i < runs; ++i)
  {
    auto const t0 = std::chrono::steady_clock::now();
    img           = popcorn::image::load_elf(elf);
    auto const t1 = std::chrono::steady_clock::now();
    times_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  std::ranges::sort(times_ms);
  auto const median = times_ms[static_cast<std::size_t>(runs) / 2];
  std::printf(
      "rows=%zu files=%zu symbols=%zu median=%.3fms min=%.3fms max=%.3fms\n", img.lines.size(),
      img.files.size(), img.symbols.size(), median, times_ms.front(), times_ms.back());
  return 0;
}

auto symbol_kind_name(popcorn::image::symbol_kind kind) -> char const*
{
  switch(kind)
  {
    case popcorn::image::symbol_kind::function:
      return "function";
    case popcorn::image::symbol_kind::object:
      return "object";
    case popcorn::image::symbol_kind::other:
      return "other";
  }
  return "other";
}

auto run_rows(std::filesystem::path const& elf) -> int
{
  auto const img = popcorn::image::load_elf(elf);
  for(auto const& row : img.lines)
  {
    auto const& file = row.file < img.files.size() ? img.files[row.file] : std::string{};
    std::printf(
        "%08x %u %.*s\n", static_cast<unsigned>(row.addr), row.line, static_cast<int>(file.size()),
        file.data());
  }
  return 0;
}

auto run_symbols(std::filesystem::path const& elf) -> int
{
  auto const img = popcorn::image::load_elf(elf);
  for(auto const& sym : img.symbols)
  {
    std::printf(
        "%08x %u %s %.*s\n", static_cast<unsigned>(sym.value), sym.size, symbol_kind_name(sym.kind),
        static_cast<int>(sym.name.size()), sym.name.data());
  }
  return 0;
}

auto run_locate(std::filesystem::path const& elf) -> int
{
  auto const  img = popcorn::image::load_elf(elf);
  std::string line;
  while(std::getline(std::cin, line))
  {
    if(line.empty())
    {
      continue;
    }
    std::uint32_t addr{};
    auto const [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), addr, 16);
    if(ec != std::errc{})
    {
      std::printf("??:0\n");
      continue;
    }
    auto const loc = img.locate(addr);
    if(loc.file.empty())
    {
      std::printf("??:0\n");
    }
    else
    {
      std::printf("%.*s:%u\n", static_cast<int>(loc.file.size()), loc.file.data(), loc.line);
    }
  }
  return 0;
}
}

int main(int argc, char** argv)
{
  if(argc < 3)
  {
    std::fprintf(
        stderr, "usage: image-probe bench <elf> <runs> | locate <elf> | rows <elf> | "
                "symbols <elf>\n");
    return 1;
  }
  std::string const           mode = argv[1];
  std::filesystem::path const elf  = argv[2];
  if(mode == "bench")
  {
    if(argc < 4)
    {
      std::fprintf(stderr, "usage: image-probe bench <elf> <runs>\n");
      return 1;
    }
    return run_bench(elf, std::atoi(argv[3]));
  }
  if(mode == "locate")
  {
    return run_locate(elf);
  }
  if(mode == "rows")
  {
    return run_rows(elf);
  }
  if(mode == "symbols")
  {
    return run_symbols(elf);
  }
  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 1;
}
