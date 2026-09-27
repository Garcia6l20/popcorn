#pragma once

#include <popcorn/core/intercept.hpp>
#include <popcorn/obs/pctr.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

namespace popcorn::bind
{
namespace py = reflex::py;

namespace detail
{
/** @brief A named code range as Python passes it: name, first address, size in bytes. */
using range_row = std::tuple<std::string, address, std::uint32_t>;
}

/** @brief A `.pctr` trace a machine streams into a file, until close(). */
class[[= py::rename{"TraceHandle"}]] trace_handle
{
public:
  /**
   * @brief Owns a writer and the sink feeding it.
   * @param writer The file writer.
   * @param sink The machine's sink into @p writer.
   */
  [[= py::skip]] trace_handle(
      std::unique_ptr<obs::pctr_writer> writer,
      core::hook_handle                 sink) noexcept;

  /**
   * @brief Removes the sink, then writes what is buffered and closes the file. Does nothing
   *        when already closed.
   * @throws format_error when a write, the flush or the close fails.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Remove the sink, then flush and close the file."}]] void close();

  /** @brief Records written so far. */
  [[= py::doc{"Records written so far, STR and TIME included."}]] [[nodiscard]] auto
      records() const noexcept -> std::uint64_t;

  /** @brief Bytes written so far, the file size once closed. */
  [[= py::doc{"Bytes accepted so far, the file size once closed."}]] [[nodiscard]] auto
      size() const noexcept -> std::uint64_t;

private:
  std::unique_ptr<obs::pctr_writer> writer_;
  core::hook_handle                 sink_;
};
}
