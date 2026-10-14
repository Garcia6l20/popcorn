#pragma once

#include <popcorn/core/intercept.hpp>

#include "alive.hpp"
#include "gil_locks.hpp"

#include <reflex/py.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief Removes an intercept, a return hook or a watch. */
class[[= py::rename{"Hook"}]] hook
{
public:
  /**
   * @brief Owns a native hook handle.
   * @param handle The handle.
   * @param host The machine's host, through which remove() reaches the engine.
   */
  [[= py::skip]] hook(core::hook_handle handle, detail::host_ref host) noexcept;
  /** @brief Takes over @p other's hook. */
  [[= py::skip]] hook(hook&& other) noexcept = default;
  /** @brief Not move-assignable: the old hook would skip retire(). */
  auto operator=(hook&&) -> hook& = delete;
  /** @brief Removes the hook through detail::retire(), safe on any thread while it runs. */
  ~hook();

  /**
   * @brief Removes the hook, at once or at the end of the slice when called from a handler.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Remove the hook."}]] void remove();

  /**
   * @brief Removes the hook through detail::retire(), from any thread and while the machine
   *        runs: at once while parked, at the next boundary while running, at the end of the
   *        slice from a handler. Its handler may still be called until then. Never throws.
   */
  [[= py::doc{"Remove the hook at once while parked, at the next boundary while running."}]] void
      retire() noexcept;

  /** @brief Whether the hook is still installed. */
  [[= py::doc{"Whether the hook is still installed."}]] [[nodiscard]] auto active() const noexcept
      -> bool;

  /**
   * @brief Moves the native handle out, leaving this hook inactive, on the caller's thread so
   *        that a removal posted to the machine's thread never writes the handle other threads
   *        read.
   * @return The handle.
   */
  [[= py::skip]] [[nodiscard]] auto take() noexcept -> core::hook_handle;

  /**
   * @brief Takes back a handle take() moved out, when its removal was refused before it was
   *        posted.
   * @param handle The handle take() returned.
   */
  [[= py::skip]] void give_back(core::hook_handle handle) noexcept;

private:
  core::hook_handle handle_;
  detail::host_ref  host_;
};

/**
 * @brief A probe: the hook that removes it and the arrivals it counted, readable from any thread
 *        while the machine runs, and after the removal.
 */
class[[= py::rename{"ProbeHook"}]] probe_hook
{
public:
  /**
   * @brief Owns a native probe.
   * @param handle The probe.
   * @param host The machine's host, through which a removal reaches the engine.
   */
  [[= py::skip]] probe_hook(core::probe_handle handle, detail::host_ref host);

  /** @brief Arrivals counted so far. */
  [[= py::doc{"Arrivals counted so far."}]] [[nodiscard]] auto count() const noexcept
      -> std::uint64_t;

  /** @brief Virtual time of the first arrival in picoseconds, since reset, None before it. */
  [[= py::doc{
      "Virtual time of the first arrival in picoseconds, None before it."}]] [[nodiscard]] auto
      first_ps() const noexcept -> std::optional<std::uint64_t>;

  /** @brief Whether the probe is still installed. */
  [[= py::doc{"Whether the probe is still installed."}]] [[nodiscard]] auto active() const noexcept
      -> bool;

  /** @brief The hook that removes the probe. */
  [[= py::skip]] [[nodiscard]] auto removal() const noexcept -> std::shared_ptr<hook> const&;

  /** @brief The counts, shared with the machine. */
  [[= py::skip]] [[nodiscard]] auto hits() const noexcept
      -> std::shared_ptr<core::probe_hits const> const&;

private:
  std::shared_ptr<hook>                   hook_;
  std::shared_ptr<core::probe_hits const> hits_;
};

/** @brief A call arriving at an intercepted address, valid only while its handler runs. */
class[[= py::rename{"InterceptCall"}]] call_view
{
public:
  /**
   * @brief Views a native call for the duration of a handler.
   * @param call The call.
   * @param alive Cleared when the handler returns.
   */
  [[= py::skip]] call_view(core::intercept_call const& call, detail::alive_flag alive) noexcept;

  /** @brief Intercepted address. */
  [[= py::doc{"Intercepted address."}]] [[nodiscard]] auto function() const -> address;

  /** @brief Address the function returns to. */
  [[= py::doc{"Address the function returns to."}]] [[nodiscard]] auto return_address() const
      -> address;

  /** @brief Calls of this intercept before this one. */
  [[= py::doc{"Calls of this intercept before this one."}]] [[nodiscard]] auto occurrence() const
      -> std::uint64_t;

  /**
   * @brief Reads the arguments under a calling convention.
   * @param kinds Kind of each argument.
   * @param abi Convention, empty for the image's.
   * @throws config_error after the handler returned.
   */
  [[= py::doc{"Argument bits under the image's calling convention, or abi."}]] [[nodiscard]] auto
      args(std::vector<arch::value_kind> const& kinds, std::optional<arch::float_abi> abi) const
      -> std::vector<std::uint64_t>;

private:
  [[nodiscard]] auto call() const -> core::intercept_call const&;

  core::intercept_call const* call_;
  detail::alive_flag          alive_;
};

/** @brief A call that returned, as a return hook sees it. */
struct[[= py::rename{"ReturnedCall"}]] returned_record
{
  /** @brief Intercepted function. */
  [[= py::readonly]] address function{};
  /** @brief Calls of this hook before this one. */
  [[= py::readonly]] std::uint64_t occurrence{};
  /** @brief Argument bits captured at entry. */
  [[= py::readonly]] std::vector<std::uint64_t> args{};
  /** @brief Return value bits. */
  [[= py::readonly]] std::uint64_t value{};
  /** @brief Instructions from entry to return. */
  [[= py::readonly]] std::uint64_t instructions{};
};

/** @brief A return a ReturnLog holds, with the virtual time it returned at. */
struct[[= py::rename{"LoggedReturn"}]] logged_return
{
  /** @brief Virtual time of the return in picoseconds. */
  [[= py::readonly]] std::uint64_t time_ps{};
  /** @brief Calls of this hook before this one. */
  [[= py::readonly]] std::uint64_t occurrence{};
  /** @brief Argument bits captured at entry. */
  [[= py::readonly]] std::vector<std::uint64_t> args{};
  /** @brief Return value bits. */
  [[= py::readonly]] std::uint64_t value{};
  /** @brief Instructions from entry to return. */
  [[= py::readonly]] std::uint64_t instructions{};
};

/**
 * @brief Returns of one function, appended by a native return hook on the machine thread without
 *        the GIL, read from any thread. Holds the newest entries up to its capacity. Its copies
 *        share one log. Every member takes a leaf mutex: nothing else is locked and no Python
 *        runs under it.
 */
class[[= py::rename{"ReturnLog"}]] return_log
{
public:
  /**
   * @brief An empty log.
   * @param capacity Entries held, at least 1.
   * @throws std::invalid_argument for a capacity of 0.
   */
  explicit return_log(std::uint64_t capacity);

  /** @brief Entries appended since the log was made, dropped and cleared ones included. */
  [[= py::doc{"Entries appended so far, a position for since()."}]] [[nodiscard]] auto mark() const
      -> std::uint64_t;

  /**
   * @brief Copies the entries appended at or after a mark and still held, oldest first.
   * @param mark A value of mark().
   */
  [[= py::doc{"Copy of the entries appended at or after mark and still held."}]] [[nodiscard]] auto
      since(std::uint64_t mark) const -> std::vector<logged_return>;

  /** @brief Entries dropped because the log held its capacity. */
  [[= py::doc{"Entries dropped because the log was full."}]] [[nodiscard]] auto dropped() const
      -> std::uint64_t;

  /** @brief Drops every entry held, mark() keeps counting. */
  [[= py::doc{"Drop every entry held, marks keep counting."}]] void clear();

  /** @brief Refuses every later append. */
  [[= py::doc{"Refuse every later append."}]] void stop();

  /**
   * @brief Appends an entry unless stopped, dropping the oldest when full.
   * @param entry The return.
   */
  [[= py::skip]] void append(logged_return entry) const;

private:
  struct state
  {
    std::mutex                mutex;
    std::deque<logged_return> entries;
    std::uint64_t             capacity = 1;
    std::uint64_t             total    = 0;
    std::uint64_t             dropped  = 0;
    bool                      stopped  = false;
  };

  std::shared_ptr<state> state_;
};
}
