#pragma once

#include <popcorn/core/machine.hpp>

#include <reflex/py.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    std::function<reflex::py::nb::object()>>;

/**
 * @brief One read or write of `Machine.read` and `Machine.write`, the body a sync accessor runs,
 *        built on the caller and run on the engine owner with no GIL. Opaque in Python.
 */
class[[= py::rename{"AccessOp"}]] access_op
{
public:
  /** @brief The body. */
  using body_type = std::function<op_value(core::machine&)>;

  /**
   * @brief Wraps @p body.
   * @param body Called with the machine, Python-free.
   */
  [[= py::skip]] explicit access_op(body_type body) : body_{std::move(body)}
  {}

  /** @brief Runs the op on @p target, as engine owner. */
  [[= py::skip]] auto run(core::machine& target) const -> op_value
  {
    return body_(target);
  }

private:
  body_type body_;
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
