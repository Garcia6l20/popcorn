#include <popcorn/obs/errors.hpp>
#include <popcorn/obs/pctr.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace popcorn::obs
{
namespace
{
static_assert(std::endian::native == std::endian::little);

constexpr std::size_t   buffer_size = std::size_t{1} << 20;
constexpr std::uint16_t version     = 1;
constexpr std::size_t   longest     = std::numeric_limits<std::uint16_t>::max();
constexpr std::uint32_t last_id     = core::trace_id_overflow - 1;

auto time_record(icount at) -> core::trace_record
{
  return {
      .kind = core::trace_kind::time,
      .payload =
          {static_cast<std::uint32_t>(at.value), static_cast<std::uint32_t>(at.value >> 32), 0},
  };
}

auto sorted(std::vector<named_range> ranges) -> std::vector<named_range>
{
  std::ranges::sort(ranges, {}, &named_range::start);
  return ranges;
}

auto find(std::vector<named_range> const& ranges, address at) -> std::string const*
{
  auto const after = std::ranges::upper_bound(ranges, at, {}, &named_range::start);
  if(after == ranges.begin())
  {
    return nullptr;
  }
  auto const& held = *std::prev(after);
  return std::uint64_t{at} < std::uint64_t{held.start} + held.size ? &held.name : nullptr;
}
}

void pctr_writer::closer::operator()(std::FILE* file) const noexcept
{
  static_cast<void>(std::fclose(file));
}

pctr_writer::pctr_writer(std::filesystem::path path, icount origin, pctr_names names)
    : path_{std::move(path)}, names_{std::move(names)}
{
  names_.register_ranges = sorted(std::move(names_.register_ranges));
  names_.symbol_ranges   = sorted(std::move(names_.symbol_ranges));
  file_.reset(std::fopen(path_.c_str(), "wb"));
  if(not file_)
  {
    throw format_error(path_.string(), 0, "a file that can be created");
  }
  buffer_.reserve(buffer_size);
  constexpr std::array magic{'P', 'C', 'T', 'R'};
  put(magic.data(), magic.size());
  put(&version, sizeof version);
  std::uint16_t const flags = 0;
  put(&flags, sizeof flags);
  record(time_record(origin));
}

pctr_writer::~pctr_writer()
{
  try
  {
    close();
  }
  catch(format_error const&)
  {}
}

void pctr_writer::put(void const* data, std::size_t size)
{
  if(buffer_.size() + size > buffer_size)
  {
    spill();
  }
  auto const* first = static_cast<std::byte const*>(data);
  buffer_.insert(buffer_.end(), first, first + size);
  bytes_ += size;
}

void pctr_writer::spill()
{
  if(buffer_.empty())
  {
    return;
  }
  if(not file_)
  {
    throw format_error(path_.string(), written_, "an open file");
  }
  auto const written = std::fwrite(buffer_.data(), 1, buffer_.size(), file_.get());
  auto const failed  = written != buffer_.size();
  written_ += written;
  buffer_.clear();
  if(failed)
  {
    throw format_error(path_.string(), written_, "room to write");
  }
}

auto pctr_writer::string_id(std::string const& name) -> std::uint16_t
{
  auto const text = name.substr(0, longest);
  if(auto const found = ids_.find(text); found != ids_.end())
  {
    return found->second;
  }
  if(ids_.size() > last_id)
  {
    return core::trace_id_overflow;
  }
  auto const id = static_cast<std::uint16_t>(ids_.size());
  ids_.emplace(text, id);
  core::trace_record const header{
      .kind = core::trace_kind::str, .aux = static_cast<std::uint16_t>(text.size()), .dt = id};
  put(&header, core::pctr_header_size);
  put(text.data(), text.size());
  constexpr std::array<std::byte, 3> zeros{};
  put(zeros.data(), -text.size() & 3);
  ++records_;
  return id;
}

auto pctr_writer::resolve(core::trace_record const& record) -> std::uint16_t
{
  auto const* names = record.kind == core::trace_kind::mmio ? &names_.registers : &names_.symbols;
  auto const* ranges =
      record.kind == core::trace_kind::mmio ? &names_.register_ranges : &names_.symbol_ranges;
  auto const  at   = record.kind == core::trace_kind::mmio ? record.payload[1] : record.payload[0];
  auto const* name = record.aux < names->size() ? &(*names)[record.aux] : find(*ranges, at);
  return name == nullptr ? core::trace_id_overflow : string_id(*name);
}

void pctr_writer::record(core::trace_record const& record)
{
  auto stamped = record;
  if(record.kind == core::trace_kind::mmio or record.kind == core::trace_kind::call)
  {
    stamped.aux = resolve(record);
  }
  put(&stamped, core::pctr_header_size + 4 * core::pctr_payload_words(record.kind));
  ++records_;
}

void pctr_writer::write(std::span<core::trace_record const> records)
{
  contract_assert(std::ranges::none_of(records, [](core::trace_record const& record) {
    return record.kind == core::trace_kind::str;
  }));
  if(not file_)
  {
    throw format_error(path_.string(), written_, "an open file");
  }
  for(auto const& held : records)
  {
    record(held);
  }
}

auto pctr_writer::sink() -> core::trace_sink
{
  return [this](std::span<core::trace_record const> records) { write(records); };
}

void pctr_writer::close()
{
  if(not file_)
  {
    return;
  }
  auto failure = std::string{};
  try
  {
    spill();
  }
  catch(format_error const&)
  {
    failure = "room to write";
  }
  if(failure.empty() and std::fflush(file_.get()) != 0)
  {
    failure = "a successful flush";
  }
  auto* const file = file_.release();
  if(std::fclose(file) != 0 and failure.empty())
  {
    failure = "a successful close";
  }
  if(not failure.empty())
  {
    throw format_error(path_.string(), written_, failure);
  }
}

auto pctr_writer::records() const noexcept -> std::uint64_t
{
  return records_;
}

auto pctr_writer::bytes() const noexcept -> std::uint64_t
{
  return bytes_;
}
}
