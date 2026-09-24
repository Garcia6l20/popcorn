#include <popcorn/core/snapshot.hpp>

#include <popcorn/core/errors.hpp>

#include <utility>

namespace popcorn::core
{
class engine_context
{};

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

auto encode_snapshot(snapshot_data const&) -> std::vector<std::byte>
{
  throw not_implemented("encode_snapshot");
}

auto decode_snapshot(std::span<std::byte const>) -> snapshot_data
{
  throw not_implemented("decode_snapshot");
}
}
