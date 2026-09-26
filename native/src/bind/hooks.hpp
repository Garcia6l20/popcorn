#pragma once

#include <popcorn/core/intercept.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <memory>
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
   */
  [[= py::skip]] explicit hook(core::hook_handle handle) noexcept;

  /**
   * @brief Removes the hook, at once or at the end of the slice when called from a handler.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[= py::doc{"Remove the hook."}]] void remove();

  /** @brief Whether the hook is still installed. */
  [[= py::doc{"Whether the hook is still installed."}]] [[nodiscard]] auto active() const noexcept
      -> bool;

private:
  core::hook_handle handle_;
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
  [[= py::skip]] call_view(core::intercept_call const& call, std::shared_ptr<bool> alive) noexcept;

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
  std::shared_ptr<bool>       alive_;
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
}
