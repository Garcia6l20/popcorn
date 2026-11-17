#pragma once

#include <popcorn/core/intercept.hpp>
#include <popcorn/obs/pctr.hpp>

#include "../obs/profile_sink.hpp"
#include "access_op.hpp"
#include "gil_locks.hpp"

#include <reflex/py.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

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
   * @param writer The file writer, shared with the machine's sink.
   * @param sink The machine's sink into @p writer, made by obs::owned_sink().
   * @param host The machine's host, through which close() removes the sink.
   */
  [[= py::skip]] trace_handle(
      std::shared_ptr<obs::pctr_writer> writer,
      core::hook_handle                 sink,
      detail::host_ref                  host) noexcept;
  /** @brief Takes over @p other's writer and sink. @p other is left empty and must not be used:
   * nanobind never exposes a moved-from handle. */
  [[= py::skip]] trace_handle(trace_handle&& other) noexcept = default;
  /** @brief Not move-assignable. */
  auto operator=(trace_handle&&) -> trace_handle& = delete;
  /**
   * @brief Drops the handle's share of the trace. The last share, the handle's or a close
   *        command's, removes the sink and drops the writer through detail::retire(), safe on
   *        any thread while the machine runs: the sink shares the writer, which lives until the
   *        machine destroyed the sink, at the slice end for a removal inside a slice.
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
        std::shared_ptr<obs::pctr_writer> writer,
        core::hook_handle                 sink,
        detail::host_ref                  host) noexcept;
    state(state const&)                    = delete;
    auto operator=(state const&) -> state& = delete;
    ~state();

    std::shared_ptr<obs::pctr_writer> writer;
    core::hook_handle                 sink;
    detail::host_ref                  host;
    std::mutex                        close_mutex;
    bool                              closed = false;
  };

  std::shared_ptr<state> state_;
};

/** @brief Options of a profile, see obs::profile_options. */
struct[[= py::rename{"ProfileOptions"}]] profile_settings
{
  /** @brief Picoseconds of the burst window, 0 for none. */
  std::uint64_t burst_window_ps{};
  /** @brief Flat register ids whose CPU writes are logged. */
  std::vector<std::uint16_t> logged_registers{};
  /** @brief Writes kept per logged register. */
  std::uint64_t write_log_limit{4096};
  /** @brief Clock domain whose rate the profile follows for the logged writes, empty or unknown
   * for none. */
  std::string write_clock_domain{};
  /** @brief Whether the call stack tracker runs. */
  bool call_stacks{};
  /** @brief Most call frames the tracker keeps. */
  std::uint32_t max_call_depth{256};
  /** @brief Fewest passes of a reported loop run the profile's sink asks for while open, 0 for
   * no request, see core::machine::add_trace_sink(). */
  std::uint32_t loop_reports{};
};

/**
 * @brief A profile sink a machine feeds until close(), then its aggregates. The sink and its
 *        hook live in one shared state, destroyed through detail::retire() with the hook removed
 *        first. The aggregates are read on the machine's thread right after the hook is removed,
 *        while no slice runs, so the sink has had every record.
 */
class[[= py::rename{"ProfileHandle"}]] profile_handle
{
public:
  /**
   * @brief Owns a sink and the hook feeding it.
   * @param sink The sink, shared with the machine's hook.
   * @param hook The machine's hook into @p sink, made by obs::owned_sink().
   * @param host The machine's host.
   */
  [[= py::skip]] profile_handle(
      std::shared_ptr<obs::profile_sink> sink,
      core::hook_handle                  hook,
      detail::host_ref                   host);
  /** @brief Takes over @p other's state. @p other is left empty and must not be used. */
  [[= py::skip]] profile_handle(profile_handle&& other) noexcept = default;
  /** @brief Not move-assignable. */
  auto operator=(profile_handle&&) -> profile_handle& = delete;
  /**
   * @brief Drops the handle's share. The last share, the handle's or a close command's, removes
   *        the hook and drops the sink through detail::retire(). The hook shares the sink, which
   *        lives until the machine destroyed the hook, at the slice end for a removal inside a
   *        slice. The removal withdraws the profile's loop report request.
   */
  ~profile_handle() = default;

  /**
   * @brief Removes the hook, which withdraws the profile's loop report request, then takes the
   *        aggregates. Does nothing when already closed.
   * @throws machine_running while a slice runs, handlers included.
   * @throws core::machine_closed once the machine was closed.
   */
  [[= py::doc{"Remove the sink, then take its aggregates."}]] void close();

  /**
   * @brief close() as a command of MachineHandle.submit_ops(), run at a boundary of a free run.
   *        It shares the profile with the handle. A command never posted leaves the profile to
   *        the handle.
   */
  [[= py::doc{"Command of close."}]] [[nodiscard]] auto command_close() -> access_op;

  /** @brief Whether close() or its command ran. */
  [[= py::doc{"Whether the profile was closed and its aggregates taken."}]] [[nodiscard]] auto
      closed() const -> bool;

  /**
   * @brief The aggregates taken at close.
   * @throws machine_running while the profile is open.
   */
  [[= py::doc{"The aggregates taken at close."}]] [[nodiscard]] auto result() const
      -> obs::profile_result;

private:
  struct state
  {
    state(
        std::shared_ptr<obs::profile_sink> sink,
        core::hook_handle                  hook,
        detail::host_ref                   host) noexcept;
    state(state const&)                    = delete;
    auto operator=(state const&) -> state& = delete;
    ~state();

    void finish();

    std::shared_ptr<obs::profile_sink> sink;
    core::hook_handle                  hook;
    detail::host_ref                   host;
    mutable std::mutex                 close_mutex;
    std::optional<obs::profile_result> result;
  };

  std::shared_ptr<state> state_;
};
}
