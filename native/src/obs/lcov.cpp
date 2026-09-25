#include "thumb.hpp"

#include <popcorn/obs/coverage.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::obs
{
namespace
{
using count = std::uint64_t;

constexpr auto most = std::numeric_limits<count>::max();

constexpr auto add(count a, count b) noexcept -> count
{
  return a > most - b ? most : a + b;
}

class instruction_counts
{
public:
  explicit instruction_counts(std::span<block_hit const> blocks)
  {
    std::map<std::uint64_t, std::pair<unsigned __int128, unsigned __int128>> events;
    for(auto const& block : blocks)
    {
      events[block.start].first += block.hits;
      events[std::uint64_t{block.start} + block.size].second += block.hits;
    }
    unsigned __int128 running = 0;
    for(auto const& [at, change] : events)
    {
      running += change.first;
      running -= change.second;
      steps_.emplace_back(at, running > most ? most : static_cast<count>(running));
    }
  }

  [[nodiscard]] auto at(address x) const noexcept -> count
  {
    auto const after = std::ranges::upper_bound(
        steps_, std::uint64_t{x}, {}, &std::pair<std::uint64_t, count>::first);
    return after == steps_.begin() ? 0 : std::prev(after)->second;
  }

private:
  std::vector<std::pair<std::uint64_t, count>> steps_;
};

struct branch_count
{
  std::optional<count> taken{};
  std::optional<count> not_taken{};
};

struct function_count
{
  std::uint32_t line{};
  count         hits{};
};

struct file_record
{
  std::map<std::uint32_t, count>                           lines;
  std::map<std::string, function_count>                    functions;
  std::map<std::uint32_t, std::map<address, branch_count>> branches;
};

using file_records = std::map<std::string, file_record>;

auto inside(std::span<address_range const> ranges, address at) noexcept -> bool
{
  return std::ranges::any_of(ranges, [at](address_range const& range) {
    return range.start <= at and std::uint64_t{at} < std::uint64_t{range.start} + range.size;
  });
}

auto executable_segment(image::image const& source, address at) -> image::segment const*
{
  auto const found = std::ranges::find_if(source.segments, [at](image::segment const& held) {
    return held.executable
       and held.start
        <= at
       and std::uint64_t{at}
         < std::uint64_t{held.start}
         + held.memory_size;
  });
  return found == source.segments.end() ? nullptr : &*found;
}

auto row_at(image::image const& source, address at) -> image::line_row const*
{
  auto const after = std::ranges::upper_bound(source.lines, at, {}, &image::line_row::addr);
  if(after == source.lines.begin())
  {
    return nullptr;
  }
  auto const& row = *std::prev(after);
  return row.line == 0 ? nullptr : &row;
}

auto is_trap(thumb::insn const& i) noexcept -> bool
{
  return (i.size == 2 and (i.bits & 0xff00) == 0xde00)
      or (i.size == 4 and (i.bits & 0xfff0'f000) == 0xf7f0'a000);
}

auto stops_fallthrough(thumb::insn const& i, thumb::flow const& control) noexcept -> bool
{
  switch(control.kind)
  {
    case thumb::ending::none:
    case thumb::ending::system:
      return false;
    case thumb::ending::exception:
      return is_trap(i);
    case thumb::ending::direct:
    case thumb::ending::indirect:
      return not control.conditional and not thumb::is_call(i);
  }
  std::unreachable();
}

class branch_finder
{
public:
  branch_finder(image::segment const& code, address low, address high)
      : code_{code}, low_{low}, high_{high}
  {}

  void walk(address root, bool known)
  {
    known = known or not after_it(root);
    pending_.push_back(
        {.at = root, .it_left = 0, .unknown_left = static_cast<std::uint8_t>(known ? 0 : 4)});
    while(not pending_.empty())
    {
      auto const item = pending_.back();
      pending_.pop_back();
      step(item);
    }
  }

  [[nodiscard]] auto found() const -> std::map<address, thumb::cond_branch> const&
  {
    return found_;
  }

private:
  [[nodiscard]] auto after_it(address root) const noexcept -> bool
  {
    for(address back = 2; back <= 16; back += 2)
    {
      if(root < code_.start + back)
      {
        break;
      }
      auto const offset = root - back - code_.start;
      if(offset + 1 >= code_.bytes.size())
      {
        continue;
      }
      auto const low  = std::to_integer<unsigned>(code_.bytes[offset]);
      auto const high = std::to_integer<unsigned>(code_.bytes[offset + 1]);
      if(high == 0xbf and (low & 0xf) != 0)
      {
        return true;
      }
    }
    return false;
  }

  struct item
  {
    address      at{};
    std::uint8_t it_left{};
    std::uint8_t unknown_left{};
  };

  void step(item const& current)
  {
    if(current.at < low_ or current.at >= high_ or current.at < code_.start)
    {
      return;
    }
    auto const known = current.unknown_left == 0;
    if(auto const seen = visited_.find(current.at);
       seen != visited_.end() and (seen->second or not known))
    {
      return;
    }
    visited_[current.at] = known;
    auto const offset    = current.at - code_.start;
    if(offset >= code_.bytes.size())
    {
      return;
    }
    auto const decoded = thumb::read(current.at, std::span{code_.bytes}.subspan(offset));
    if(not decoded)
    {
      return;
    }
    auto const predicated = current.it_left > 0 or current.unknown_left > 0;
    if(not predicated)
    {
      if(auto const branch = thumb::conditional_branch(*decoded))
      {
        found_.emplace(branch->at, *branch);
      }
    }
    auto const control = thumb::classify(*decoded);
    auto const opens   = thumb::is_it(*decoded);
    auto const next    = item{
        .at      = current.at + decoded->size,
        .it_left = opens ? thumb::it_length(*decoded)
                         : static_cast<std::uint8_t>(current.it_left > 0 ? current.it_left - 1 : 0),
        .unknown_left = opens ? std::uint8_t{0}
                              : static_cast<std::uint8_t>(
                                    current.unknown_left > 0 ? current.unknown_left - 1 : 0),
    };
    if(control.kind == thumb::ending::direct and control.target and not thumb::is_call(*decoded))
    {
      pending_.push_back({.at = *control.target, .it_left = 0, .unknown_left = 0});
    }
    if(predicated or not stops_fallthrough(*decoded, control))
    {
      pending_.push_back(next);
    }
  }

  image::segment const&                 code_;
  address                               low_;
  address                               high_;
  std::vector<item>                     pending_;
  std::map<address, bool>               visited_;
  std::map<address, thumb::cond_branch> found_;
};

auto function_end(
    image::image const&   source,
    image::symbol const&  function,
    image::segment const& code) -> address
{
  auto const segment_end = std::uint64_t{code.start} + code.memory_size;
  if(function.size != 0)
  {
    return static_cast<address>(
        std::min(std::uint64_t{function.value} + function.size, segment_end));
  }
  auto const later =
      std::ranges::upper_bound(source.symbols, function.value, {}, &image::symbol::value);
  auto const next =
      std::ranges::find_if(later, source.symbols.end(), [&](image::symbol const& candidate) {
        return candidate.kind == image::symbol_kind::function;
      });
  return next == source.symbols.end() or next->value >= segment_end
           ? static_cast<address>(segment_end)
           : next->value;
}

class group_view
{
public:
  explicit group_view(coverage_group const& group) : group_{group}, counts_{group.blocks}
  {
    for(auto const& edge : group.edges)
    {
      edges_[{edge.from, edge.to}] = edge.hits;
    }
    for(auto const& block : group.blocks)
    {
      by_end_[std::uint64_t{block.start} + block.size].push_back(&block);
    }
  }

  [[nodiscard]] auto in_scope(address at) const noexcept -> bool
  {
    return inside(group_.ranges, at);
  }

  [[nodiscard]] auto instructions(address at) const noexcept -> count
  {
    return counts_.at(at);
  }

  [[nodiscard]] auto entries(address at) const -> count
  {
    count total = 0;
    for(auto const& block : std::ranges::equal_range(group_.blocks, at, {}, &block_hit::start))
    {
      total = add(total, block.hits);
    }
    return total;
  }

  [[nodiscard]] auto outcome(thumb::cond_branch const& branch) const -> branch_count
  {
    auto const end       = std::uint64_t{branch.at} + branch.size;
    auto       ran       = false;
    count      taken     = 0;
    count      not_taken = 0;
    auto const ending    = by_end_.find(end);
    if(ending == by_end_.end())
    {
      return {};
    }
    for(auto const* block_ptr : ending->second)
    {
      auto const& block = *block_ptr;
      if(block.start > branch.at)
      {
        continue;
      }
      ran   = ran or block.hits != 0;
      taken = add(taken, edge(block.start, branch.taken));
      if(branch.taken != branch.fallthrough)
      {
        not_taken = add(not_taken, edge(block.start, branch.fallthrough));
      }
    }
    if(not ran)
    {
      return {};
    }
    return {.taken = taken, .not_taken = not_taken};
  }

  [[nodiscard]] auto blocks() const noexcept -> std::span<block_hit const>
  {
    return group_.blocks;
  }

  [[nodiscard]] auto blocks_between(address low, address high) const noexcept
      -> std::span<block_hit const>
  {
    auto const first = std::ranges::lower_bound(group_.blocks, low, {}, &block_hit::start);
    auto const last  = std::ranges::lower_bound(group_.blocks, high, {}, &block_hit::start);
    return {first, last};
  }

private:
  [[nodiscard]] auto edge(address from, address to) const -> count
  {
    auto const found = edges_.find({from, to});
    return found == edges_.end() ? 0 : found->second;
  }

  coverage_group const&                                  group_;
  instruction_counts                                     counts_;
  std::map<std::pair<address, address>, count>           edges_;
  std::map<std::uint64_t, std::vector<block_hit const*>> by_end_;
};

void add_lines(group_view const& view, image::image const& source, file_records& out)
{
  auto const keep = [&](image::line_row const& row, count value) {
    auto& held = out[source.files.at(row.file)].lines[row.line];
    held       = std::max(held, value);
  };
  for(auto const& [index, row] : std::views::enumerate(source.lines))
  {
    auto const next  = static_cast<std::size_t>(index) + 1;
    auto const empty = next < source.lines.size() and source.lines[next].addr == row.addr;
    if(row.line != 0 and not empty and view.in_scope(row.addr))
    {
      keep(row, view.instructions(row.addr));
    }
  }
  for(auto const& block : view.blocks())
  {
    if(not view.in_scope(block.start) or executable_segment(source, block.start) == nullptr)
    {
      continue;
    }
    if(auto const* row = row_at(source, block.start))
    {
      keep(*row, view.instructions(block.start));
    }
  }
}

auto function_row(image::image const& source, address at) -> image::line_row const*
{
  auto const* entry = row_at(source, at);
  if(entry == nullptr)
  {
    return nullptr;
  }
  auto const same = std::ranges::equal_range(source.lines, at, {}, &image::line_row::addr);
  image::line_row const* best = entry;
  for(auto const& row : same)
  {
    if(row.file == entry->file and row.line != 0 and row.line < best->line)
    {
      best = &row;
    }
  }
  return best;
}

void add_functions(group_view const& view, image::image const& source, file_records& out)
{
  for(auto const& function : source.symbols)
  {
    if(function.kind
       != image::symbol_kind::function
       or not view.in_scope(function.value)
       or executable_segment(source, function.value)
       == nullptr)
    {
      continue;
    }
    if(auto const* row = function_row(source, function.value))
    {
      auto const hits          = view.entries(function.value);
      auto const [held, fresh] = out[source.files.at(row->file)].functions.try_emplace(
          function.name, function_count{.line = row->line, .hits = hits});
      if(not fresh)
      {
        held->second.hits = add(held->second.hits, hits);
      }
    }
  }
}

void add_branches(group_view const& view, image::image const& source, file_records& out)
{
  for(auto const& function : source.symbols)
  {
    if(function.kind != image::symbol_kind::function or not view.in_scope(function.value))
    {
      continue;
    }
    auto const* code = executable_segment(source, function.value);
    if(code == nullptr)
    {
      continue;
    }
    auto const    low  = function.value;
    auto const    high = function_end(source, function, *code);
    branch_finder finder{*code, low, high};
    finder.walk(low, true);
    auto const first_row = std::ranges::lower_bound(source.lines, low, {}, &image::line_row::addr);
    auto const last_row  = std::ranges::lower_bound(source.lines, high, {}, &image::line_row::addr);
    for(auto const& row : std::ranges::subrange(first_row, last_row))
    {
      if(row.line != 0)
      {
        finder.walk(row.addr, false);
      }
    }
    for(auto const& block : view.blocks_between(low, high))
    {
      finder.walk(block.start, false);
    }
    for(auto const& [at, branch] : finder.found())
    {
      if(not view.in_scope(at))
      {
        continue;
      }
      if(auto const* row = row_at(source, at))
      {
        out[source.files.at(row->file)].branches[row->line][at] = view.outcome(branch);
      }
    }
  }
}

auto merge_optional(std::optional<count> into, std::optional<count> from) -> std::optional<count>
{
  if(not into)
  {
    return from;
  }
  if(not from)
  {
    return into;
  }
  return add(*into, *from);
}

void merge_group(file_records const& group, file_records& total)
{
  for(auto const& [path, record] : group)
  {
    auto& into = total[path];
    for(auto const& [line, value] : record.lines)
    {
      into.lines[line] = add(into.lines[line], value);
    }
    for(auto const& [name, function] : record.functions)
    {
      auto const [held, fresh] = into.functions.try_emplace(name, function);
      if(not fresh)
      {
        held->second.hits = add(held->second.hits, function.hits);
      }
    }
    for(auto const& [line, by_address] : record.branches)
    {
      auto& held = into.branches[line];
      for(auto const& [index, entry] : std::views::enumerate(by_address))
      {
        auto const& branch = entry.second;
        auto const  key    = static_cast<address>(index);
        auto&       slot   = held[key];
        slot.taken         = merge_optional(slot.taken, branch.taken);
        slot.not_taken     = merge_optional(slot.not_taken, branch.not_taken);
      }
    }
  }
}

auto text(std::optional<count> value) -> std::string
{
  return value ? std::format("{}", *value) : std::string{"-"};
}

void write_file(
    std::string const&  path,
    file_record const&  record,
    lcov_options const& options,
    std::string&        out)
{
  std::format_to(std::back_inserter(out), "TN:{}\nSF:{}\n", options.test_name, path);
  std::vector<std::pair<std::uint32_t, std::string_view>> order;
  for(auto const& [name, function] : record.functions)
  {
    order.emplace_back(function.line, name);
  }
  std::ranges::sort(order);
  for(auto const& [line, name] : order)
  {
    std::format_to(std::back_inserter(out), "FN:{},{}\n", line, name);
  }
  for(auto const& [line, name] : order)
  {
    std::format_to(
        std::back_inserter(out), "FNDA:{},{}\n", record.functions.at(std::string{name}).hits, name);
  }
  std::format_to(
      std::back_inserter(out), "FNF:{}\nFNH:{}\n", record.functions.size(),
      std::ranges::count_if(
          record.functions, [](auto const& entry) { return entry.second.hits != 0; }));
  if(options.branches)
  {
    std::size_t found = 0;
    std::size_t hit   = 0;
    for(auto const& [line, by_index] : record.branches)
    {
      for(auto const& [index, entry] : std::views::enumerate(by_index))
      {
        auto const& branch = entry.second;
        for(auto const& [side, value] : std::array{
                std::pair{0, branch.taken    },
                std::pair{1, branch.not_taken}
        })
        {
          std::format_to(
              std::back_inserter(out), "BRDA:{},{},{},{}\n", line, index, side, text(value));
          ++found;
          hit += value.value_or(0) != 0 ? 1 : 0;
        }
      }
    }
    std::format_to(std::back_inserter(out), "BRF:{}\nBRH:{}\n", found, hit);
  }
  for(auto const& [line, value] : record.lines)
  {
    std::format_to(std::back_inserter(out), "DA:{},{}\n", line, value);
  }
  std::format_to(
      std::back_inserter(out), "LF:{}\nLH:{}\nend_of_record\n", record.lines.size(),
      std::ranges::count_if(record.lines, [](auto const& entry) { return entry.second != 0; }));
}
}

auto lcov(
    coverage_set const&                        set,
    std::span<std::vector<image::image> const> images,
    lcov_options const&                        options) -> std::string
{
  file_records total;
  for(auto const& [group, loaded] : std::views::zip(set.groups, images))
  {
    contract_assert(loaded.size() == group.images.size());
    group_view   view{group};
    file_records records;
    for(auto const& [ref, source] : std::views::zip(group.images, loaded))
    {
      if(ref.kind != image_kind::elf)
      {
        continue;
      }
      add_lines(view, source, records);
      add_functions(view, source, records);
      add_branches(view, source, records);
    }
    merge_group(records, total);
  }
  std::string out;
  for(auto const& [path, record] : total)
  {
    write_file(path, record, options, out);
  }
  return out;
}
}
