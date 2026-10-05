#include "models.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/model.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/trampoline.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::bind::detail
{
namespace
{
namespace nb = reflex::py::nb;

auto register_name(peripheral_desc const& desc, std::uint32_t offset) -> std::string
{
  auto const found = std::ranges::find_if(desc.registers, [&](register_desc const& reg) {
    return offset >= reg.offset and offset < reg.offset + std::max(reg.width / 8u, 1u);
  });
  return found == desc.registers.end() ? std::string{} : found->name;
}

/**
 * @brief nanobind trampoline letting a Python class implement periph::model through the private
 *        methods `_claims`, `_reset`, `_read`, `_write`, `_event`, `_save` and `_load` that
 *        `popcorn.model.Model` defines. Every call takes the GIL. A bus error crosses as a
 *        return value: None from `_read` and False from `_write` become bus_error::denied.
 */
class python_model final : public periph::model
{
public:
  NB_TRAMPOLINE(periph::model, 7);

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    nb::gil_scoped_acquire gil;
    return nb::cast<std::vector<std::string>>(call("_claims"));
  }

  void reset(periph::register_file&, reset_kind kind) override
  {
    nb::gil_scoped_acquire gil;
    call("_reset", kind);
  }

  auto on_read(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      periph::access_ctx) -> std::expected<std::uint32_t, bus_error> override
  {
    nb::gil_scoped_acquire gil;
    auto const             result = call("_read", offset, size);
    if(result.is_none())
    {
      return std::unexpected{bus_error::denied};
    }
    std::uint32_t value{};
    if(not nb::isinstance<nb::int_>(result) or not nb::try_cast(result, value))
    {
      throw model_error(regs.description().name, register_name(regs.description(), offset), offset);
    }
    return value;
  }

  auto on_write(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      std::uint32_t          old,
      std::uint32_t          value,
      periph::access_ctx) -> std::expected<void, bus_error> override
  {
    nb::gil_scoped_acquire gil;
    auto const             result = call("_write", offset, size, old, value);
    if(result.is_none())
    {
      return {};
    }
    if(not nb::isinstance<nb::bool_>(result))
    {
      throw model_error(regs.description().name, register_name(regs.description(), offset), offset);
    }
    if(not nb::cast<bool>(result))
    {
      return std::unexpected{bus_error::denied};
    }
    return {};
  }

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    nb::gil_scoped_acquire gil;
    call("_event", kind, payload);
  }

  void save(std::vector<std::byte>& out) const override
  {
    nb::gil_scoped_acquire gil;
    auto const             bytes = nb::cast<nb::bytes>(call("_save"));
    auto const             data  = static_cast<std::byte const*>(bytes.data());
    out.insert(out.end(), data, data + bytes.size());
  }

  void load(std::span<std::byte const> in) override
  {
    nb::gil_scoped_acquire gil;
    call("_load", nb::bytes(in.data(), in.size()));
  }

private:
  template <typename... Args> auto call(char const* name, Args&&... args) const -> nb::object
  {
    nb::detail::ticket ticket(nb_trampoline, name, true);
    return nb_trampoline.base().attr(ticket.key)(std::forward<Args>(args)...);
  }
};

thread_local std::shared_ptr<bool const> building{};

auto current_alive() -> std::shared_ptr<bool const>
{
  if(not building)
  {
    throw config_error("models", 0, "python", "a Python model built with its machine from Python");
  }
  return building;
}

auto to_model(nb::object const& made, peripheral_desc const& desc) -> std::shared_ptr<periph::model>
{
  std::shared_ptr<periph::model> model;
  if(not nb::try_cast(made, model) or not model)
  {
    throw config_error(
        desc.name, 0, "model",
        std::format(
            "a popcorn.model.Model instance, not {}", nb::cast<std::string>(nb::repr(made))));
  }
  return model;
}
}

void bind_model_base(nb::module_ scope)
{
  nb::class_<periph::model, python_model>(
      scope, "ModelBase",
      "Native side of popcorn.model.Model: a peripheral model implemented in Python.")
      .def(nb::init<>());
}

auto python_model_factory(nb::object maker) -> periph::model_factory
{
  return
      [callable = held(std::move(maker))](periph::model_context& ctx, peripheral_desc const& desc) {
        auto                   alive = current_alive();
        nb::gil_scoped_acquire gil;
        return to_model(callable->fn(model_context_view{ctx, std::move(alive)}), desc);
      };
}

auto python_object(periph::model* model) -> nb::object
{
  auto* const python = dynamic_cast<python_model*>(model);
  if(python == nullptr)
  {
    return nb::none();
  }
  return nb::find(static_cast<periph::model*>(python));
}

building_scope::building_scope(std::shared_ptr<bool const> alive) noexcept
    : previous_{std::exchange(building, std::move(alive))}
{}

building_scope::~building_scope()
{
  building = std::move(previous_);
}
}

namespace popcorn::bind
{
void register_python_model(std::string const& ip, std::string const& version_glob)
{
  periph::register_model(
      ip, version_glob,
      [ip, version_glob](periph::model_context& ctx, peripheral_desc const& desc) {
        auto                               alive = detail::current_alive();
        reflex::py::nb::gil_scoped_acquire gil;
        auto const makers = reflex::py::nb::module_::import_("popcorn.model").attr("_REGISTERED");
        auto const maker  = makers[reflex::py::nb::make_tuple(ip, version_glob)];
        return detail::to_model(maker(model_context_view{ctx, std::move(alive)}), desc);
      });
}

void unregister_python_model(std::string const& ip, std::string const& version_glob)
{
  periph::register_model(ip, version_glob, {});
}

auto registered_model_glob(std::string const& ip, std::string const& ip_version)
    -> std::optional<std::string>
{
  return periph::registered_glob(ip, ip_version);
}
}
