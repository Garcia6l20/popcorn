#include "snapshot.hpp"

#include <nanobind/nanobind.h>

#include <cstddef>
#include <span>
#include <utility>

namespace popcorn::bind
{
snapshot_handle::snapshot_handle(core::snapshot saved) : saved_{std::move(saved)}
{}

auto snapshot_handle::machine_id() const noexcept -> std::uint64_t
{
  return saved_.machine_id();
}

auto snapshot_handle::format() const noexcept -> std::uint32_t
{
  return saved_.data().format;
}

auto snapshot_handle::description_digest() const -> std::string
{
  return saved_.data().description_digest;
}

auto snapshot_handle::image_digests() const -> std::vector<std::string>
{
  return saved_.data().image_digests;
}

auto snapshot_handle::config_digest() const -> std::string
{
  return saved_.data().config_digest;
}

auto snapshot_handle::generation() const noexcept -> std::uint64_t
{
  return saved_.data().generation;
}

auto snapshot_handle::encode() const -> reflex::py::nb::bytes
{
  std::vector<std::byte> bytes;
  {
    reflex::py::nb::gil_scoped_release unlocked;
    bytes = core::encode_snapshot(saved_.data());
  }
  return reflex::py::nb::bytes(bytes.data(), bytes.size());
}

auto snapshot_handle::native() const noexcept -> core::snapshot const&
{
  return saved_;
}

auto decode_snapshot(reflex::py::nb::bytes const& bytes) -> snapshot_handle
{
  auto const view = std::span{static_cast<std::byte const*>(bytes.data()), bytes.size()};
  reflex::py::nb::gil_scoped_release unlocked;
  return snapshot_handle{core::snapshot{core::decode_snapshot(view)}};
}
}
