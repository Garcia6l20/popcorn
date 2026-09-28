#include <popcorn/core/snapshot.hpp>

#include "engine.hpp"
#include "engine_context.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/serde/bson.hpp>

#include <format>
#include <stdexcept>
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
  if(data.format == 2)
  {
    data.format = snapshot_format_version;
  }
  if(data.format != snapshot_format_version)
  {
    throw config_error(
        "snapshot", 0, "format",
        std::format(
            "snapshot_format_version {}, bytes have {}", snapshot_format_version, data.format));
  }
  return data;
}
}
