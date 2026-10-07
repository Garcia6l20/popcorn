#pragma once

#include "casters.hpp"

#include <popcorn/core/snapshot.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief A machine snapshot, immutable, restorable any number of times. */
class[[= py::rename{"SnapshotHandle"}]] snapshot_handle
{
public:
  /**
   * @brief Wraps a native snapshot.
   * @param saved The snapshot.
   */
  [[= py::skip]] explicit snapshot_handle(core::snapshot saved);

  /** @brief Identity of the machine that took it, 0 for one read back from bytes. */
  [[= py::doc{
      "Identity of the machine that took it, 0 for one read back from bytes."}]] [[nodiscard]] auto
      machine_id() const noexcept -> std::uint64_t;

  /** @brief Layout version. */
  [[= py::doc{"Layout version of the saved form."}]] [[nodiscard]] auto format() const noexcept
      -> std::uint32_t;

  /** @brief Digest of the description it was taken on. */
  [[= py::doc{"Digest of the description it was taken on."}]] [[nodiscard]] auto
      description_digest() const -> std::string;

  /** @brief Digest of each image loaded when it was taken, in load order. */
  [[= py::doc{"Digest of each image loaded when it was taken, in load order."}]] [[nodiscard]] auto
      image_digests() const -> std::vector<std::string>;

  /** @brief Configuration digest of the machine that took it. */
  [[= py::doc{"Configuration digest of the machine that took it."}]] [[nodiscard]] auto
      config_digest() const -> std::string;

  /** @brief Machine generation when it was taken. */
  [[= py::doc{"Machine generation when it was taken."}]] [[nodiscard]] auto
      generation() const noexcept -> std::uint64_t;

  /** @brief The saved form, encode_snapshot() bytes. */
  [[= py::doc{"The saved form, BSON bytes that decode_snapshot reads back."}]] [[nodiscard]] auto
      encode() const -> reflex::py::nb::bytes;

  /** @brief The native snapshot. */
  [[= py::skip]] [[nodiscard]] auto native() const noexcept -> core::snapshot const&;

private:
  core::snapshot saved_;
};

/**
 * @brief Reads a saved form back.
 * @param bytes encode() bytes.
 * @throws config_error when the bytes are malformed or of another format.
 */
[[= py::doc{"Read a saved snapshot back, ConfigError when malformed or of another format."}]] auto
    decode_snapshot(reflex::py::nb::bytes const& bytes) -> snapshot_handle;
}
