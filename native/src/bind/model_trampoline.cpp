#include <popcorn/periph/model.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/trampoline.h>

#include <expected>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace popcorn::bind
{
/**
 * @brief nanobind trampoline letting a Python class implement periph::model. The register file
 *        crosses as a pointer, which nanobind passes by reference without taking ownership. A
 *        bus error crosses as a return value: `on_read` returns the value or None, `on_write`
 *        returns None or True, or False. None from `on_read` and False from `on_write` become
 *        bus_error::denied.
 */
class py_model : public periph::model
{
public:
  NB_TRAMPOLINE(periph::model, 7);

  /** @brief Destroys the trampoline. Out of line: the key function emits every override. */
  ~py_model() override;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return call<std::vector<std::string>>("claims");
  }

  void reset(periph::register_file& regs, reset_kind kind) override
  {
    call<void>("reset", &regs, kind);
  }

  auto on_read(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      periph::access_ctx     ctx) -> std::expected<std::uint32_t, bus_error> override
  {
    nb::gil_scoped_acquire gil;
    auto const             result = call<nb::object>("on_read", &regs, offset, size, ctx);
    if(result.is_none())
    {
      return std::unexpected{bus_error::denied};
    }
    return nb::cast<std::uint32_t>(result);
  }

  auto on_write(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      std::uint32_t          old,
      std::uint32_t          value,
      periph::access_ctx     ctx) -> std::expected<void, bus_error> override
  {
    nb::gil_scoped_acquire gil;
    auto const result = call<nb::object>("on_write", &regs, offset, size, old, value, ctx);
    if(not result.is_none() and not nb::cast<bool>(result))
    {
      return std::unexpected{bus_error::denied};
    }
    return {};
  }

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    call<void>("on_event", kind, payload);
  }

  void save(std::vector<std::byte>& out) const override
  {
    nb::gil_scoped_acquire gil;
    auto const             bytes = call<nb::bytes>("save");
    auto const             data  = static_cast<std::byte const*>(bytes.data());
    out.insert(out.end(), data, data + bytes.size());
  }

  void load(std::span<std::byte const> in) override
  {
    nb::gil_scoped_acquire gil;
    call<void>("load", nb::bytes(in.data(), in.size()));
  }

private:
  template <typename R, typename... Args> auto call(char const* name, Args&&... args) const -> R
  {
    nb::detail::ticket ticket(nb_trampoline, name, true);
    auto               result = nb_trampoline.base().attr(ticket.key)(std::forward<Args>(args)...);
    if constexpr(not std::is_void_v<R>)
    {
      return nb::cast<R>(result);
    }
  }
};

py_model::~py_model() = default;
}
