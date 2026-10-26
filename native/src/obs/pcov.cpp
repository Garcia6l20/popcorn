#include <popcorn/image/image.hpp>
#include <popcorn/obs/coverage.hpp>
#include <popcorn/obs/errors.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <tuple>
#include <utility>

#include <unistd.h>

namespace popcorn::obs
{
format_error::format_error(std::string file, std::uint64_t offset, std::string expected)
    : error("format error: {}: offset {}: expected {}", file, offset, expected),
      file_(std::move(file)), offset_(offset), expected_(std::move(expected))
{}

format_error::~format_error() = default;

auto format_error::file() const noexcept -> std::string const&
{
  return file_;
}

auto format_error::offset() const noexcept -> std::uint64_t
{
  return offset_;
}

auto format_error::expected() const noexcept -> std::string const&
{
  return expected_;
}

namespace
{
static_assert(std::endian::native == std::endian::little);

constexpr std::array<char, 4> magic{'P', 'C', 'O', 'V'};
constexpr std::uint16_t       version        = 2;
constexpr std::uint16_t       has_unmodelled = 1;
constexpr std::uint32_t       largest_tb     = 1024;
constexpr auto                most           = std::numeric_limits<std::uint64_t>::max();

constexpr auto add(std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t
{
  return a > most - b ? most : a + b;
}

auto digests(coverage_group const& group) -> std::vector<std::string>
{
  return group.images | std::views::transform(&image_ref::digest) | std::ranges::to<std::vector>();
}

auto range_key(address_range const& range)
{
  return std::tie(range.start, range.size, range.name);
}

auto block_key(block_hit const& block)
{
  return std::pair{block.start, block.size};
}

auto edge_key(edge_hit const& edge)
{
  return std::pair{edge.from, edge.to};
}

auto image_key(image_ref const& ref)
{
  return std::tie(ref.path, ref.kind, ref.base, ref.end);
}

auto unmodelled_key(core::unmodelled_access const& access)
{
  return std::tie(
      access.pc, access.peripheral, access.reg, access.offset, access.write, access.reason);
}

using unmodelled_tuple =
    std::tuple<address, std::string, std::string, std::uint32_t, bool, core::unmodelled_reason>;

struct accumulated
{
  std::vector<image_ref>                                     images;
  std::map<std::pair<address, std::uint32_t>, std::string>   ranges;
  std::map<std::pair<address, std::uint32_t>, std::uint64_t> blocks;
  std::map<std::pair<address, address>, std::uint64_t>       edges;
  std::map<unmodelled_tuple, std::uint64_t>                  unmodelled;
  std::uint64_t                                              split{};
  std::uint64_t                                              dropped{};
};

void fold(accumulated& into, coverage_group const& group)
{
  if(into.images.empty())
  {
    into.images = group.images;
  }
  else
  {
    for(auto const& [held, other] : std::views::zip(into.images, group.images))
    {
      if(image_key(other) < image_key(held))
      {
        held = other;
      }
    }
  }
  for(auto const& range : group.ranges)
  {
    auto const [held, fresh] = into.ranges.try_emplace({range.start, range.size}, range.name);
    if(not fresh and range.name < held->second)
    {
      held->second = range.name;
    }
  }
  for(auto const& block : group.blocks)
  {
    auto& held = into.blocks[block_key(block)];
    held       = add(held, block.hits);
  }
  for(auto const& edge : group.edges)
  {
    auto& held = into.edges[edge_key(edge)];
    held       = add(held, edge.hits);
  }
  for(auto const& access : group.unmodelled)
  {
    auto& held = into.unmodelled[unmodelled_tuple{unmodelled_key(access)}];
    held       = add(held, access.hits);
  }
  into.split   = add(into.split, group.split);
  into.dropped = add(into.dropped, group.dropped);
}

auto finish(accumulated const& held) -> coverage_group
{
  coverage_group group{.images = held.images, .split = held.split, .dropped = held.dropped};
  for(auto const& [key, name] : held.ranges)
  {
    group.ranges.push_back({.name = name, .start = key.first, .size = key.second});
  }
  std::ranges::sort(group.ranges, {}, range_key);
  for(auto const& [key, hits] : held.blocks)
  {
    if(hits != 0)
    {
      group.blocks.push_back({.start = key.first, .size = key.second, .hits = hits});
    }
  }
  for(auto const& [key, hits] : held.edges)
  {
    if(hits != 0)
    {
      group.edges.push_back({.from = key.first, .to = key.second, .hits = hits});
    }
  }
  for(auto const& [key, hits] : held.unmodelled)
  {
    if(hits != 0)
    {
      auto const& [pc, peripheral, reg, offset, write, reason] = key;
      group.unmodelled.push_back(
          {.pc         = pc,
           .peripheral = peripheral,
           .reg        = reg,
           .offset     = offset,
           .write      = write,
           .reason     = reason,
           .hits       = hits});
    }
  }
  return group;
}

class writer
{
public:
  void u16(std::uint16_t value)
  {
    raw(&value, sizeof value);
  }
  void u32(std::uint32_t value)
  {
    raw(&value, sizeof value);
  }
  void u64(std::uint64_t value)
  {
    raw(&value, sizeof value);
  }
  void text(std::string_view value)
  {
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value.data(), value.size());
    bytes_.resize(bytes_.size() + (-value.size() & 3), std::byte{0});
  }
  void raw(void const* data, std::size_t size)
  {
    auto const* first = static_cast<std::byte const*>(data);
    bytes_.insert(bytes_.end(), first, first + size);
  }
  [[nodiscard]] auto take() -> std::vector<std::byte>
  {
    return std::move(bytes_);
  }

private:
  std::vector<std::byte> bytes_;
};

class reader
{
public:
  reader(std::span<std::byte const> bytes, std::string_view file) : bytes_{bytes}, file_{file}
  {}

  template <typename T> auto scalar(std::string_view what) -> T
  {
    need(sizeof(T), what);
    T value{};
    std::memcpy(&value, bytes_.data() + at_, sizeof(T));
    at_ += sizeof(T);
    return value;
  }

  auto text(std::string_view what) -> std::string
  {
    auto const size   = scalar<std::uint32_t>(what);
    auto const padded = std::uint64_t{size} + (-std::uint64_t{size} & 3);
    need(padded, what);
    std::string value(reinterpret_cast<char const*>(bytes_.data() + at_), size);
    for(auto index = at_ + size; index < at_ + padded; ++index)
    {
      if(bytes_[index] != std::byte{0})
      {
        fail(index, std::format("zero padding after {}", what));
      }
    }
    at_ += static_cast<std::size_t>(padded);
    return value;
  }

  auto count(std::size_t record, std::string_view what) -> std::uint32_t
  {
    auto const found = scalar<std::uint32_t>(what);
    need(std::uint64_t{found} * record, what);
    return found;
  }

  [[noreturn]] void fail(std::uint64_t offset, std::string expected) const
  {
    throw format_error(std::string{file_}, offset, std::move(expected));
  }

  [[nodiscard]] auto offset() const noexcept -> std::uint64_t
  {
    return at_;
  }

  [[nodiscard]] auto done() const noexcept -> bool
  {
    return at_ == bytes_.size();
  }

private:
  void need(std::uint64_t size, std::string_view what) const
  {
    if(size > bytes_.size() - at_)
    {
      fail(at_, std::format("{}, {} bytes, before the end of the file", what, size));
    }
  }

  std::span<std::byte const> bytes_;
  std::string_view           file_;
  std::size_t                at_ = 0;
};

auto decode_group(reader& in, bool unmodelled) -> coverage_group
{
  coverage_group group;
  group.split       = in.scalar<std::uint64_t>("the split count");
  group.dropped     = in.scalar<std::uint64_t>("the dropped count");
  auto const images = in.count(20, "the image count");
  for(std::uint32_t index = 0; index < images; ++index)
  {
    auto const at   = in.offset();
    auto const kind = in.scalar<std::uint32_t>("an image kind");
    if(kind > std::to_underlying(image_kind::raw))
    {
      in.fail(at, "an image kind of 0 or 1");
    }
    image_ref ref{.kind = static_cast<image_kind>(kind)};
    ref.base   = in.scalar<std::uint32_t>("an image base");
    ref.end    = in.scalar<std::uint32_t>("an image end");
    ref.path   = in.text("an image path");
    ref.digest = in.text("an image digest");
    if(ref.kind == image_kind::raw and not ref.path.empty())
    {
      in.fail(at, "an empty path for a raw image");
    }
    group.images.push_back(std::move(ref));
  }
  auto const ranges = in.count(12, "the range count");
  for(std::uint32_t index = 0; index < ranges; ++index)
  {
    auto const    at = in.offset();
    address_range range;
    range.start = in.scalar<std::uint32_t>("a range start");
    range.size  = in.scalar<std::uint32_t>("a range size");
    range.name  = in.text("a range name");
    if(not group.ranges.empty()
       and std::pair{group.ranges.back().start, group.ranges.back().size}
               >= std::pair{range.start, range.size})
    {
      in.fail(at, "ranges sorted by start and size, each once");
    }
    group.ranges.push_back(std::move(range));
  }
  auto const blocks = in.count(16, "the block count");
  for(std::uint32_t index = 0; index < blocks; ++index)
  {
    auto const at = in.offset();
    block_hit  block{
        .start = in.scalar<std::uint32_t>("a block start"),
        .size  = in.scalar<std::uint32_t>("a block size"),
        .hits  = in.scalar<std::uint64_t>("a block hit count")};
    if(block.size == 0 or block.size > largest_tb)
    {
      in.fail(at, std::format("a block size of 1 to {}", largest_tb));
    }
    if(block.hits == 0)
    {
      in.fail(at, "a block hit count above 0");
    }
    if(not group.blocks.empty() and block_key(group.blocks.back()) >= block_key(block))
    {
      in.fail(at, "blocks sorted by start and size, each once");
    }
    group.blocks.push_back(block);
  }
  auto const edges = in.count(16, "the edge count");
  for(std::uint32_t index = 0; index < edges; ++index)
  {
    auto const at = in.offset();
    edge_hit   edge{
        .from = in.scalar<std::uint32_t>("an edge source"),
        .to   = in.scalar<std::uint32_t>("an edge target"),
        .hits = in.scalar<std::uint64_t>("an edge hit count")};
    if(edge.hits == 0)
    {
      in.fail(at, "an edge hit count above 0");
    }
    if(not group.edges.empty() and edge_key(group.edges.back()) >= edge_key(edge))
    {
      in.fail(at, "edges sorted by source and target, each once");
    }
    group.edges.push_back(edge);
  }
  if(not unmodelled)
  {
    return group;
  }
  auto const accesses = in.count(28, "the unmodelled access count");
  for(std::uint32_t index = 0; index < accesses; ++index)
  {
    auto const              at = in.offset();
    core::unmodelled_access access{.pc = in.scalar<std::uint32_t>("an unmodelled access pc")};
    access.offset     = in.scalar<std::uint32_t>("an unmodelled access offset");
    auto const kind   = in.scalar<std::uint32_t>("an unmodelled access kind");
    access.hits       = in.scalar<std::uint64_t>("an unmodelled access hit count");
    access.peripheral = in.text("an unmodelled access peripheral");
    access.reg        = in.text("an unmodelled access register");
    auto const reason = kind >> 8;
    if((kind & ~std::uint32_t{0xff01}) != 0
       or reason < std::to_underlying(core::unmodelled_reason::no_register)
       or reason > std::to_underlying(core::unmodelled_reason::raz_wi))
    {
      in.fail(
          at,
          std::format(
              "an unmodelled access kind of write bit 0 and a reason of {} to {} in bits 8 to 15",
              std::to_underlying(core::unmodelled_reason::no_register),
              std::to_underlying(core::unmodelled_reason::raz_wi)));
    }
    access.write  = (kind & 1) != 0;
    access.reason = static_cast<core::unmodelled_reason>(reason);
    if(access.hits == 0)
    {
      in.fail(at, "an unmodelled access hit count above 0");
    }
    if(not group.unmodelled.empty()
       and unmodelled_key(group.unmodelled.back()) >= unmodelled_key(access))
    {
      in.fail(
          at, "unmodelled accesses sorted by pc, peripheral, register, offset, write and reason, "
              "each once");
    }
    group.unmodelled.push_back(std::move(access));
  }
  return group;
}

auto read_file(std::filesystem::path const& path) -> std::vector<std::byte>
{
  std::ifstream in{path, std::ios::binary};
  if(not in)
  {
    throw format_error(path.string(), 0, "a readable file");
  }
  std::vector<char> const chars{std::istreambuf_iterator<char>{in}, {}};
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}
}

auto merge(std::span<coverage_set const> sets) -> coverage_set
{
  std::map<std::vector<std::string>, accumulated> groups;
  for(auto const& set : sets)
  {
    for(auto const& group : set.groups)
    {
      fold(groups[digests(group)], group);
    }
  }
  coverage_set merged;
  for(auto const& held : groups | std::views::values)
  {
    merged.groups.push_back(finish(held));
  }
  return merged;
}

auto encode_pcov(coverage_set const& set) -> std::vector<std::byte>
{
  auto const canonical  = merge(std::span{&set, 1});
  auto const unmodelled = std::ranges::any_of(
      canonical.groups, [](coverage_group const& group) { return not group.unmodelled.empty(); });
  writer out;
  out.raw(magic.data(), magic.size());
  out.u16(version);
  out.u16(unmodelled ? has_unmodelled : 0);
  out.u32(static_cast<std::uint32_t>(canonical.groups.size()));
  for(auto const& group : canonical.groups)
  {
    out.u64(group.split);
    out.u64(group.dropped);
    out.u32(static_cast<std::uint32_t>(group.images.size()));
    for(auto const& ref : group.images)
    {
      out.u32(std::to_underlying(ref.kind));
      out.u32(ref.base);
      out.u32(ref.end);
      out.text(ref.path);
      out.text(ref.digest);
    }
    out.u32(static_cast<std::uint32_t>(group.ranges.size()));
    for(auto const& range : group.ranges)
    {
      out.u32(range.start);
      out.u32(range.size);
      out.text(range.name);
    }
    out.u32(static_cast<std::uint32_t>(group.blocks.size()));
    for(auto const& block : group.blocks)
    {
      out.u32(block.start);
      out.u32(block.size);
      out.u64(block.hits);
    }
    out.u32(static_cast<std::uint32_t>(group.edges.size()));
    for(auto const& edge : group.edges)
    {
      out.u32(edge.from);
      out.u32(edge.to);
      out.u64(edge.hits);
    }
    if(unmodelled)
    {
      out.u32(static_cast<std::uint32_t>(group.unmodelled.size()));
      for(auto const& access : group.unmodelled)
      {
        out.u32(access.pc);
        out.u32(access.offset);
        out.u32(
            std::uint32_t{std::to_underlying(access.reason)} << 8 | std::uint32_t{access.write});
        out.u64(access.hits);
        out.text(access.peripheral);
        out.text(access.reg);
      }
    }
  }
  return out.take();
}

auto decode_pcov(std::span<std::byte const> bytes, std::string_view file) -> coverage_set
{
  reader     in{bytes, file};
  auto const head = in.scalar<std::array<char, 4>>("the PCOV magic");
  if(head != magic)
  {
    in.fail(0, "the PCOV magic");
  }
  auto const found = in.scalar<std::uint16_t>("the version");
  if(found != 1 and found != version)
  {
    in.fail(4, "version 1 or 2");
  }
  auto const flags = in.scalar<std::uint16_t>("the flags");
  if(found == 1 and flags != 0)
  {
    in.fail(6, "flags 0 in version 1");
  }
  if((flags & ~has_unmodelled) != 0)
  {
    in.fail(6, "flags 0 or 1");
  }
  auto const   groups = in.count(0, "the group count");
  coverage_set set;
  for(std::uint32_t index = 0; index < groups; ++index)
  {
    auto const at    = in.offset();
    auto       group = decode_group(in, (flags & has_unmodelled) != 0);
    if(not set.groups.empty() and digests(set.groups.back()) >= digests(group))
    {
      in.fail(at, "groups sorted by their image digests, each once");
    }
    set.groups.push_back(std::move(group));
  }
  if((flags & has_unmodelled) != 0
     and std::ranges::all_of(
         set.groups, [](coverage_group const& group) { return group.unmodelled.empty(); }))
  {
    in.fail(6, "flags bit 0 only with an unmodelled access");
  }
  if(not in.done())
  {
    in.fail(in.offset(), "the end of the file");
  }
  return set;
}

void save_pcov(std::filesystem::path const& path, coverage_set const& set)
{
  auto const bytes     = encode_pcov(set);
  auto       temporary = path;
  temporary += std::format(".tmp.{}", ::getpid());
  {
    std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
    out.write(
        reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if(not out)
    {
      throw format_error(temporary.string(), 0, "a writable file");
    }
  }
  std::error_code failed;
  std::filesystem::rename(temporary, path, failed);
  if(failed)
  {
    throw format_error(
        path.string(), 0, std::format("a file to rename onto, {}", failed.message()));
  }
}

auto load_pcov(std::filesystem::path const& path) -> coverage_set
{
  return decode_pcov(read_file(path), path.string());
}

auto merge_into(std::filesystem::path const& path, coverage_set const& set) -> coverage_set
{
  auto merged = std::filesystem::exists(path) ? merge(std::array{load_pcov(path), set})
                                              : merge(std::span{&set, 1});
  save_pcov(path, merged);
  return merged;
}

auto load_images(coverage_set const& set) -> std::vector<std::vector<image::image>>
{
  std::vector<std::vector<image::image>> images;
  for(auto const& group : set.groups)
  {
    auto& loaded = images.emplace_back();
    for(auto const& ref : group.images)
    {
      if(ref.kind == image_kind::raw)
      {
        loaded.emplace_back();
        continue;
      }
      if(not std::filesystem::is_regular_file(ref.path))
      {
        throw format_error(ref.path, 0, "the ELF the coverage was collected against");
      }
      auto elf = image::load_elf(ref.path);
      if(elf.digest != ref.digest)
      {
        throw format_error(
            ref.path, 0,
            std::format("an ELF with digest {}, it changed since collect", ref.digest));
      }
      loaded.push_back(std::move(elf));
    }
  }
  return images;
}
}
