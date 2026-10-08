#pragma once

#include <popcorn/core/intercept.hpp>
#include <popcorn/obs/pctr.hpp>

#include "access_op.hpp"
#include "gil_locks.hpp"

#include <reflex/py.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
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
   * @param host The machine's host, through which close() removes the sink.
   */
  [[= py::skip]] trace_handle(
      std::unique_ptr<obs::pctr_writer> writer,
      core::hook_handle                 sink,
      detail::host_ref                  host) noexcept;
  /** @brief Takes over @p other's writer and sink. @p other is left empty and must not be used:
   * nanobind never exposes a moved-from handle. */
  [[= py::skip]] trace_handle(trace_handle&& other) noexcept = default;
  /** @brief Not move-assignable. */
  auto operator=(trace_handle&&) -> trace_handle& = delete;
  /**
   * @brief Drops the handle's share of the trace. The last share, the handle's or a close
   *        command's, removes the sink and destroys the writer through detail::retire(), safe on
   *        any thread while the machine runs: the writer lives until its sink is gone.
   */
  ~trace_handle() = default;

  /**
   * @brief Removes the sink, then writes what is buffered and closes the file. Does nothing
   *        when already closed.
   * @throws format_error when a write, the flush or the close fails.
   * @throws machine_running while a slice runs, handlers included.
   */
  [[= py::doc{"Remove the sink, then flush and close the file."}]] void close();

  /**
   * @brief close() as a command of MachineHandle.submit_ops(): it shares the trace with the
   *        handle and closes the file when it runs. A command never posted leaves the trace to
   *        the handle.
   */
  [[= py::doc{"Command of close."}]] [[nodiscard]] auto command_close() -> access_op;

  /** @brief Records written so far. */
  [[= py::doc{"Records written so far, STR and TIME included."}]] [[nodiscard]] auto
      records() const noexcept -> std::uint64_t;

  /** @brief Bytes written so far, the file size once closed. */
  [[= py::doc{"Bytes accepted so far, the file size once closed."}]] [[nodiscard]] auto
      size() const noexcept -> std::uint64_t;

private:
  struct state
  {
    state(
        std::unique_ptr<obs::pctr_writer> writer,
        core::hook_handle                 sink,
        detail::host_ref                  host) noexcept;
    state(state const&)                    = delete;
    auto operator=(state const&) -> state& = delete;
    ~state();

    std::unique_ptr<obs::pctr_writer> writer;
    core::hook_handle                 sink;
    detail::host_ref                  host;
    std::mutex                        close_mutex;
    bool                              closed = false;
  };

  std::shared_ptr<state> state_;
};
}
