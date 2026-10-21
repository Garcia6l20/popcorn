#pragma once

#include "gil_locks.hpp"

#include <popcorn/core/machine.hpp>

#include <reflex/py.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/**
 * @brief What one access op returns, native data only: nothing, an integer, a bool, bytes, or a
 *        conversion of native data to Python, run with the GIL once the command completed.
 */
using op_value = std::variant<
    std::monostate,
    std::uint64_t,
    bool,
    std::vector<std::byte>,
    std::move_only_function<reflex::py::nb::object() const>>;

/**
 * @brief One read or write of `Machine.read` and `Machine.write`, the body a sync accessor runs,
 *        built on the caller and run on the engine owner with no GIL. Opaque in Python.
 */
class[[= py::rename{"AccessOp"}]] access_op
{
public:
  /** @brief The body. */
  using body_type = std::copyable_function<op_value(core::machine&)>;

  /**
   * @brief Wraps @p body.
   * @param body Called with the machine, Python-free.
   */
  template <typename Body>
    requires(
        not std::same_as<std::remove_cvref_t<Body>, access_op>
        and std::constructible_from<body_type, Body>)
  [[= py::skip]] explicit access_op(Body&& body) : body_{std::forward<Body>(body)}
  {}

  /** @brief Runs the op on @p target, as engine owner. */
  [[= py::skip]] auto run(core::machine& target) -> op_value
  {
    return body_(target);
  }

private:
  body_type body_;
};

/**
 * @brief A condition popcorn waits for, Python-free: evaluated with no GIL on the machine's
 *        thread at the slice ends of a free run, or after every event of a parked run. Opaque in
 *        Python.
 */
class[[= py::rename{"Predicate"}]] native_predicate
{
public:
  /** @brief The test, copied into every waiter and goal that uses the predicate. */
  using test_type = std::copyable_function<bool() const>;

  /**
   * @brief Wraps @p test.
   * @param test Reads engine or link state as engine owner, Python-free.
   * @param machine The machine's slot, through which holds() reaches the engine.
   */
  [[= py::skip]] native_predicate(test_type test, detail::host_ref machine)
      : test_{std::move(test)}, machine_{std::move(machine)}
  {}

  /**
   * @brief Whether the condition holds now, evaluated on this thread with the machine parked.
   * @throws machine_running while the machine runs.
   */
  [[= py::doc{"Whether it holds now, the machine parked."}]] [[nodiscard]] auto holds() const
      -> bool
  {
    return detail::host_access(machine_, test_);
  }

  /** @brief The test, for a waiter or a goal. */
  [[= py::skip]] [[nodiscard]] auto test() const -> test_type const&
  {
    return test_;
  }

  /** @brief What the condition waits for, in an error message. */
  [[= py::doc{"What the condition waits for, for error messages."}]] [[nodiscard]] auto what() const
      -> std::string
  {
    return what_;
  }

  /** @brief This predicate, waiting for @p what. */
  [[= py::skip]] [[nodiscard]] auto described(std::string what) && -> native_predicate
  {
    what_ = std::move(what);
    return std::move(*this);
  }

private:
  test_type        test_;
  detail::host_ref machine_;
  std::string      what_ = "a condition";
};

/**
 * @brief A count a native predicate compares a queue's size against, set from Python and read
 *        with no GIL on the machine's thread.
 */
class[[= py::rename{"CountMark"}]] count_mark
{
public:
  /** @brief Starts at 0. */
  count_mark() : count_{std::make_shared<std::atomic<std::uint64_t>>(0)}
  {}

  /** @brief Stores @p count. */
  [[= py::doc{"Set the count."}]] void set(std::uint64_t count) noexcept
  {
    count_->store(count, std::memory_order_release);
  }

  /** @brief The shared count, for a predicate. */
  [[= py::skip]] [[nodiscard]] auto shared() const
      -> std::shared_ptr<std::atomic<std::uint64_t> const>
  {
    return count_;
  }

private:
  std::shared_ptr<std::atomic<std::uint64_t>> count_;
};

namespace detail
{
/** @brief The op_value of what an accessor body returns. */
template <typename T> auto as_op_value(T&& value) -> op_value
{
  using bare = std::remove_cvref_t<T>;
  if constexpr(std::same_as<bare, bool>)
  {
    return value;
  }
  else if constexpr(std::is_enum_v<bare>)
  {
    return static_cast<std::uint64_t>(std::to_underlying(value));
  }
  else if constexpr(std::integral<bare>)
  {
    return static_cast<std::uint64_t>(value);
  }
  else
  {
    return op_value{std::forward<T>(value)};
  }
}

/**
 * @brief An op over a body taking the machine, the same body its sync accessor runs.
 * @param body Called with core::machine&, returns void, an integer, an enum, a bool or bytes.
 */
template <typename Body> auto machine_op(Body body) -> access_op
{
  return access_op{[body = std::move(body)](core::machine& target) mutable -> op_value {
    if constexpr(std::is_void_v<std::invoke_result_t<Body&, core::machine&>>)
    {
      body(target);
      return std::monostate{};
    }
    else
    {
      return as_op_value(body(target));
    }
  }};
}

/**
 * @brief An op over a body taking no argument, the same body a link's sync accessor runs.
 * @param body Called with no argument, returns what machine_op() accepts.
 */
template <typename Body> auto link_op(Body body) -> access_op
{
  return machine_op([body = std::move(body)](core::machine&) mutable { return body(); });
}
}
}
