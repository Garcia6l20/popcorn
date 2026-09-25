#include <popcorn/obs/coverage.hpp>
#include <popcorn/obs/errors.hpp>

#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace popcorn::obs
{
namespace
{
constexpr std::size_t digest_prefix = 12;

auto module_path(image_ref const& ref) -> std::string
{
  if(ref.kind == image_kind::raw)
  {
    return "raw-" + ref.digest.substr(0, digest_prefix);
  }
  return ref.path;
}

auto holds(image_ref const& ref, address at) noexcept -> bool
{
  return ref.base <= at and at < ref.end;
}

template <typename T> void put(std::vector<std::byte>& out, T value)
{
  auto const at = out.size();
  out.resize(at + sizeof value);
  std::memcpy(out.data() + at, &value, sizeof value);
}
}

auto drcov(coverage_set const& set) -> drcov_file
{
  std::vector<image_ref const*>                               modules;
  std::set<std::tuple<std::uint32_t, address, std::uint32_t>> entries;
  drcov_file                                                  file;
  for(auto const& group : set.groups)
  {
    auto const first = modules.size();
    for(auto const& ref : group.images)
    {
      modules.push_back(&ref);
    }
    for(auto const& block : group.blocks)
    {
      std::optional<std::uint32_t> module;
      for(std::size_t index = 0; index < group.images.size(); ++index)
      {
        if(holds(group.images[index], block.start))
        {
          module = static_cast<std::uint32_t>(first + index);
        }
      }
      if(not module)
      {
        ++file.dropped;
        continue;
      }
      entries.emplace(*module, block.start, block.size);
    }
  }
  if(modules.size() > std::size_t{std::numeric_limits<std::uint16_t>::max()} + 1)
  {
    throw format_error("", 0, "at most 65536 drcov modules");
  }

  std::string header = std::format(
      "DRCOV VERSION: 2\nDRCOV FLAVOR: drcov\nModule Table: version 2, count {}\n"
      "Columns: id, base, end, entry, checksum, timestamp, path\n",
      modules.size());
  for(std::size_t id = 0; id < modules.size(); ++id)
  {
    auto const& ref = *modules[id];
    std::format_to(
        std::back_inserter(header), "{:2}, {:#018x}, {:#018x}, {:#018x}, {:#010x}, {:#010x}, {}\n",
        id, ref.base, ref.end, 0, 0, 0, module_path(ref));
  }
  std::format_to(std::back_inserter(header), "BB Table: {} bbs\n", entries.size());

  auto const* text = reinterpret_cast<std::byte const*>(header.data());
  file.bytes.assign(text, text + header.size());
  for(auto const& [module, start, size] : entries)
  {
    put(file.bytes, static_cast<std::uint32_t>(start - modules[module]->base));
    put(file.bytes, static_cast<std::uint16_t>(size));
    put(file.bytes, static_cast<std::uint16_t>(module));
  }
  return file;
}
}
