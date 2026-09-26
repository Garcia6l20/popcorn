#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace popcorn::core
{
/** @brief Table of a machine's intercepts, return hooks, watches, trace sinks and log sinks,
 * private to the machine. */
class hook_registry;

/**
 * @brief Owner of one intercept, return hook, watch, trace sink or log sink: removes it on
 *        destruction or reset(). Move-only. Outliving the machine is safe, the removal is then a
 *        no-op. Removing a hook changes the translated code of a running slice, so reset()
 *        refuses it while a slice runs, handlers included. Destruction and move assignment
 *        cannot refuse: during a slice, from a handler or a garbage collector, their removal
 *        takes effect at the end of the slice: until then its handler may still be called in
 *        that slice, so whatever the handler captures must outlive the slice. A restore keeps
 *        hooks: the hook table is configuration, not machine state.
 */
class hook_handle
{
public:
  /** @brief A handle owning nothing. */
  hook_handle() noexcept = default;
  /** @brief Not copyable. */
  hook_handle(hook_handle const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(hook_handle const&) -> hook_handle& = delete;
  /** @brief Takes over the hook of @p other, which then owns nothing. */
  hook_handle(hook_handle&& other) noexcept;
  /** @brief Removes the owned hook as the destructor does, then takes over the hook of
   * @p other. */
  auto operator=(hook_handle&& other) noexcept -> hook_handle&;
  /** @brief Removes the owned hook, at the end of the slice when one runs. */
  ~hook_handle();

  /**
   * @brief Removes the owned hook now. A no-op when the handle owns nothing or the machine is
   *        gone.
   * @throws machine_running while the machine runs a slice, handlers included.
   */
  void reset();

  /** @brief Whether the handle owns a hook of a live machine. */
  [[nodiscard]] auto active() const noexcept -> bool;

private:
  friend class machine;

  hook_handle(std::weak_ptr<hook_registry> registry, std::uint64_t id) noexcept;

  void release() noexcept;

  std::weak_ptr<hook_registry> registry_{};
  std::uint64_t                id_{};
};

/** @brief Owner of one intercept or return hook. */
using intercept_handle = hook_handle;

/** @brief Owner of one watch. */
using watch_handle = hook_handle;

/** @brief What the run loop does after an intercept handler returned. */
enum class[[= reflex::derive{reflex::Format}]] intercept_action
{
  proceed,
  returns,
  stop,
};

/**
 * @brief Outcome of an intercept handler: run the function body, skip it and return to the
 *        caller, or stop the run. Built only through proceed(), returns() and stop().
 */
class intercept_result
{
public:
  /** @brief Run the intercepted function as if no intercept were there. */
  [[nodiscard]] static constexpr auto proceed() noexcept -> intercept_result
  {
    return intercept_result{intercept_action::proceed, std::nullopt, std::nullopt, std::nullopt};
  }

  /**
   * @brief Skip the function body and return to the caller.
   * @param value Return value, none for a void function.
   * @param abi Calling convention the value is placed under, empty for the image's. For a
   *        function whose `pcs` attribute differs from the image, which DWARF does not record.
   */
  [[nodiscard]] static constexpr auto
      returns(std::optional<arch::call_arg> value, std::optional<arch::float_abi> abi = {}) noexcept
      -> intercept_result
  {
    return intercept_result{intercept_action::returns, value, std::nullopt, abi};
  }

  /**
   * @brief Stop the run at the intercepted address, before the function body.
   * @param reason Reason the run reports, never stop_reason::fault.
   */
  [[nodiscard]] static constexpr auto stop(stop_reason reason) noexcept -> intercept_result
      pre(is_enumerator(reason)) pre(reason != stop_reason::fault)
  {
    return intercept_result{intercept_action::stop, std::nullopt, reason, std::nullopt};
  }

  /** @brief What the run loop does. */
  [[nodiscard]] constexpr auto action() const noexcept -> intercept_action
  {
    return action_;
  }

  /** @brief Return value of a returns() result, empty for a void return and other actions. */
  [[nodiscard]] constexpr auto value() const noexcept -> std::optional<arch::call_arg> const&
  {
    return value_;
  }

  /** @brief Reason of a stop() result, empty for other actions. */
  [[nodiscard]] constexpr auto reason() const noexcept -> std::optional<stop_reason>
  {
    return reason_;
  }

  /** @brief Calling convention of a returns() result, empty for the image's and other actions. */
  [[nodiscard]] constexpr auto abi() const noexcept -> std::optional<arch::float_abi>
  {
    return abi_;
  }

private:
  constexpr intercept_result(
      intercept_action               action,
      std::optional<arch::call_arg>  value,
      std::optional<stop_reason>     reason,
      std::optional<arch::float_abi> abi) noexcept
      : action_{action}, value_{value}, reason_{reason}, abi_{abi}
  {}

  intercept_action               action_;
  std::optional<arch::call_arg>  value_{};
  std::optional<stop_reason>     reason_;
  std::optional<arch::float_abi> abi_;
};

/**
 * @brief A call reaching an intercepted address, as the handler sees it. The core is stopped at
 *        the first instruction of the function, so arguments are still in place. Valid only
 *        during the handler.
 */
class intercept_call
{
public:
  /** @brief Intercepted address. */
  [[nodiscard]] auto function() const noexcept -> address;

  /** @brief Address the function returns to, `arch::arch::return_address()`. */
  [[nodiscard]] auto return_address() const noexcept -> address;

  /** @brief Calls of this intercept before this one, the occurrence index of scripted
   * replies. */
  [[nodiscard]] auto occurrence() const noexcept -> std::uint64_t;

  /**
   * @brief Reads the arguments, `arch::arch::read_args()`.
   * @param kinds Type of each argument in declaration order, the function's signature.
   * @param abi Calling convention to read them under, empty for the image's. For a function
   *        whose `pcs` attribute differs from the image, which DWARF does not record.
   * @return Value bits of each argument, a float as its IEEE 754 bit pattern.
   */
  [[nodiscard]] auto
      args(std::span<arch::value_kind const> kinds, std::optional<arch::float_abi> abi = {}) const
      -> std::vector<std::uint64_t>;

  /** @brief Machine the call runs on, stopped at an instruction boundary for the handler. */
  [[nodiscard]] auto target() const noexcept -> machine&;

private:
  friend class machine;

  intercept_call(
      machine&      target,
      address       function,
      address       return_address,
      std::uint64_t occurrence) noexcept;

  machine*      machine_;
  address       function_;
  address       return_address_;
  std::uint64_t occurrence_;
};

/**
 * @brief Handles a call to an intercepted address. Runs synchronously on the thread that runs
 *        the slice. An exception is stored, the slice stops and `machine::run_slice` rethrows it.
 */
using intercept_handler = std::function<intercept_result(intercept_call const& call)>;

/** @brief A call that returned, as a return hook sees it. */
struct returned_call
{
  /** @brief Intercepted function. */
  address function{};
  /** @brief Calls of this hook before this one. */
  std::uint64_t occurrence{};
  /** @brief Argument bits captured at entry, in the order of the hook's argument kinds. */
  std::vector<std::uint64_t> args{};
  /** @brief Return value bits, of the hook's return kind. */
  std::uint64_t value{};
  /** @brief Instructions executed from entry to return, handlers and nested calls included. */
  icount instructions{};
};

/**
 * @brief Handles the return of an intercepted function. May read memory and registers, cannot
 *        change the return. An exception is stored and rethrown as for intercept_handler.
 */
using return_handler = std::function<void(returned_call const& call)>;

/** @brief How often an intercept fires. */
enum class[[= reflex::derive{reflex::Format}]] intercept_mode
{
  every,
  once,
};

/** @brief Accesses a watch reports. */
enum class[[= reflex::derive{reflex::Format}]] watch_kind
{
  read,
  write,
  access,
};

/** @brief One access that hit a watch. */
struct watch_hit
{
  /** @brief Address of the instruction that made the access, exact since an active watch holds
   * the machine's exact pc hook, or, for a DMA access, the pc at which the model programming
   * the transfer ran, with the precision of access_ctx::pc. */
  address pc{};
  /** @brief Accessed address. */
  address addr{};
  /** @brief Access size in bytes. */
  std::uint8_t size{};
  /** @brief Value read or written, right-aligned to size. */
  std::uint32_t value{};
  /** @brief Whether the access is a write. */
  bool write{};
  /** @brief Bus master that made the access. */
  initiator by{initiator::cpu};
};

/** @brief What the run loop does after a watch handler returned. */
enum class[[= reflex::derive{reflex::Format}]] watch_action
{
  resume,
  stop,
};

/**
 * @brief Handles a watch hit. A stop is deferred to the next block boundary: the rest of the
 *        accessing block runs, and the stop's pc is that boundary. The run then reports
 *        stop_reason::watchpoint. An exception is stored and rethrown as for
 *        intercept_handler.
 */
using watch_handler = std::function<watch_action(watch_hit const& hit)>;
}
