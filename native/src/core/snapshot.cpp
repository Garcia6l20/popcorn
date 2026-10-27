#include <popcorn/core/snapshot.hpp>

#include "engine.hpp"
#include "engine_context.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/serde/bson.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace popcorn::core
{
engine_context::engine_context(uc_struct* engine)
{
  check(uc_context_alloc(engine, &context_), "allocating an engine context");
  if(auto const err = uc_context_save(engine, context_); err != UC_ERR_OK)
  {
    uc_context_free(context_);
    check(err, "saving the engine context");
  }
}

engine_context::~engine_context()
{
  uc_context_free(context_);
}

auto engine_context::get() const noexcept -> uc_context*
{
  return context_;
}

snapshot::snapshot(snapshot_data data) : data_{std::move(data)}
{}

snapshot::snapshot(
    snapshot_data                         data,
    std::shared_ptr<engine_context const> context,
    std::uint64_t                         machine_id)
    : data_{std::move(data)}, context_{std::move(context)}, machine_id_{machine_id}
{}

auto snapshot::data() const noexcept -> snapshot_data const&
{
  return data_;
}

auto snapshot::has_engine_context() const noexcept -> bool
{
  return context_ != nullptr;
}

auto snapshot::machine_id() const noexcept -> std::uint64_t
{
  return machine_id_;
}

auto encode_snapshot(snapshot_data const& data) -> std::vector<std::byte>
{
  std::vector<std::byte> out;
  reflex::serde::bson::serializer{out}.dump(data);
  return out;
}

auto decode_snapshot(std::span<std::byte const> bytes) -> snapshot_data
{
  constexpr std::string_view format_key{"format"};
  constexpr std::size_t      element_at = 4;
  constexpr std::size_t      value_at   = element_at + 1 + format_key.size() + 1;
  constexpr std::uint8_t     int64_type = 0x12;
  constexpr std::size_t      value_end  = value_at + sizeof(std::int64_t);
  bool const                 leads_with_format =
      bytes.size() >= value_end
      and static_cast<std::uint8_t>(bytes[element_at]) == int64_type
      and std::memcmp(bytes.data() + element_at + 1, "format", format_key.size() + 1) == 0;
  if(not leads_with_format)
  {
    throw config_error(
        "snapshot", 0, "bytes", "a snapshot matching snapshot_data: no leading format element");
  }
  std::int64_t leading{};
  std::memcpy(&leading, bytes.data() + value_at, sizeof leading);
  if(leading != snapshot_format_version)
  {
    throw config_error(
        "snapshot", 0, "format",
        std::format("snapshot_format_version {}, bytes have {}", snapshot_format_version, leading));
  }
  snapshot_data data;
  try
  {
    data = reflex::serde::bson::deserializer{bytes}.load<snapshot_data>();
  }
  catch(std::runtime_error const& failure)
  {
    throw config_error(
        "snapshot", 0, "bytes",
        std::format("a snapshot matching snapshot_data: {}", failure.what()));
  }
  catch(std::out_of_range const& failure)
  {
    throw config_error(
        "snapshot", 0, "bytes",
        std::format("a snapshot matching snapshot_data: {}", failure.what()));
  }
  return data;
}
}
