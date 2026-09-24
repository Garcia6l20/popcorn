#include <popcorn/core/errors.hpp>

#include <reflex/enum.hpp>

#include <format>
#include <ranges>
#include <stdexcept>
#include <type_traits>

namespace popcorn
{
error::~error() = default;

namespace
{
auto join(std::vector<std::string> const& values) -> std::string
{
  return values | std::views::join_with(std::string_view{", "}) | std::ranges::to<std::string>();
}

auto safe_name(fault_kind kind) -> std::string
{
  try
  {
    return std::string(reflex::enum_value_name(kind));
  }
  catch(std::runtime_error const&)
  {
    return std::format("unknown({})", static_cast<std::underlying_type_t<fault_kind>>(kind));
  }
}

auto describe(
    address          pc,
    std::string_view symbol,
    std::uint32_t    offset,
    std::string_view file,
    std::uint32_t    line) -> std::string
{
  std::string text = std::format("{:#010x}", pc);
  if(offset != 0)
  {
    text += std::format("+{:#x}", offset);
  }
  if(!symbol.empty())
  {
    text += std::format(" ({})", symbol);
  }
  if(!file.empty())
  {
    text += std::format(" at {}:{}", file, line);
  }
  return text;
}

auto describe_symbol(std::string const& name, std::vector<std::string> const& close_matches)
    -> std::string
{
  if(close_matches.empty())
  {
    return std::format("unknown symbol {}", name);
  }
  return std::format("unknown symbol {}, close matches: {}", name, join(close_matches));
}
}

config_error::config_error(
    std::string   file,
    std::uint32_t line,
    std::string   key,
    std::string   expected)
    : error("config error: {}:{}: {}, expected {}", file, line, key, expected),
      file_(std::move(file)), line_(line), key_(std::move(key)), expected_(std::move(expected))
{}

auto config_error::file() const noexcept -> std::string const&
{
  return file_;
}

auto config_error::line() const noexcept -> std::uint32_t
{
  return line_;
}

auto config_error::key() const noexcept -> std::string const&
{
  return key_;
}

auto config_error::expected() const noexcept -> std::string const&
{
  return expected_;
}

image_error::image_error(std::string segment, address addr, std::string region)
    : error("image error: segment {}, address {:#010x}, {}", segment, addr, region),
      segment_(std::move(segment)), addr_(addr), region_(std::move(region))
{}

auto image_error::segment() const noexcept -> std::string const&
{
  return segment_;
}

auto image_error::addr() const noexcept -> address
{
  return addr_;
}

auto image_error::region() const noexcept -> std::string const&
{
  return region_;
}

symbol_error::symbol_error(std::string name, std::vector<std::string> close_matches)
    : error(describe_symbol(name, close_matches)), name_(std::move(name)),
      close_matches_(std::move(close_matches))
{}

auto symbol_error::name() const noexcept -> std::string const&
{
  return name_;
}

auto symbol_error::close_matches() const noexcept -> std::vector<std::string> const&
{
  return close_matches_;
}

fault_error::fault_error(location where, fault_info fault)
    : emulation_error(
          "fault {} at {}: status {:#010x}, detail {:#010x}, fault address {:#010x}",
          safe_name(fault.kind),
          describe(where.pc, where.symbol, where.offset, where.file, where.line),
          fault.status,
          fault.detail,
          fault.fault_address),
      where_(to_owned(where)), fault_(fault)
{}

auto fault_error::where() const noexcept -> owned_location const&
{
  return where_;
}

auto fault_error::fault() const noexcept -> fault_info const&
{
  return fault_;
}

budget_exceeded::budget_exceeded(location where, icount budget)
    : emulation_error(
          "budget of {} instructions exceeded at {}",
          budget.value,
          describe(where.pc, where.symbol, where.offset, where.file, where.line)),
      where_(to_owned(where)), budget_(budget)
{}

auto budget_exceeded::where() const noexcept -> owned_location const&
{
  return where_;
}

auto budget_exceeded::budget() const noexcept -> icount
{
  return budget_;
}

model_error::model_error(std::string peripheral, std::string reg, address offset)
    : error("model error: {}.{} at offset {:#06x}", peripheral, reg, offset),
      peripheral_(std::move(peripheral)), reg_(std::move(reg)), offset_(offset)
{}

auto model_error::peripheral() const noexcept -> std::string const&
{
  return peripheral_;
}

auto model_error::reg() const noexcept -> std::string const&
{
  return reg_;
}

auto model_error::offset() const noexcept -> address
{
  return offset_;
}
}
