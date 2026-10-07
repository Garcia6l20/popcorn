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
#include <optional>
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
 *        Once its machine's Python side is collected no method is called: the run is asked to
 *        stop, a read returns 0, `claims` and `save` nothing, the others have no effect.
 */
class python_model final : public periph::model
{
public:
  NB_TRAMPOLINE(periph::model, 7);

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    gil_entry const gil;
    auto const      result = call("_claims");
    return result ? nb::cast<std::vector<std::string>>(*result) : std::vector<std::string>{};
  }

  void reset(periph::register_file&, reset_kind kind) override
  {
    gil_entry const gil;
    call("_reset", kind);
  }

  auto on_read(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      periph::access_ctx) -> std::expected<std::uint32_t, bus_error> override
  {
    gil_entry const gil;
    auto const      called = call("_read", offset, size);
    if(not called)
    {
      return 0u;
    }
    auto const& result = *called;
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
    gil_entry const gil;
    auto const      called = call("_write", offset, size, old, value);
    if(not called)
    {
      return {};
    }
    auto const& result = *called;
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
    gil_entry const gil;
    call("_event", kind, payload);
  }

  void save(std::vector<std::byte>& out) const override
  {
    gil_entry const gil;
    auto const      result = call("_save");
    if(not result)
    {
      return;
    }
    auto const bytes = nb::cast<nb::bytes>(*result);
    auto const data  = static_cast<std::byte const*>(bytes.data());
    out.insert(out.end(), data, data + bytes.size());
  }

  void load(std::span<std::byte const> in) override
  {
    gil_entry const gil;
    call("_load", nb::bytes(in.data(), in.size()));
  }

  /**
   * @brief Attaches every call to the machine whose model this is.
   * @param cell A weak reference to the machine's cell, or None, the GIL held.
   * @param alive The machine's lifetime flag.
   * @param collected The machine's collected flag.
   * @param services The machine's services.
   */
  void run_in(
      nb::object              cell,
      alive_flag              alive,
      alive_flag              collected,
      core::machine_services& services)
  {
    cell_.run_in(std::move(cell), std::move(alive), std::move(collected));
    cell_.stop_through(services);
  }

private:
  template <typename... Args>
  auto call(char const* name, Args&&... args) const -> std::optional<nb::object>
  {
    return cell_.call([&] {
      nb::detail::ticket ticket(nb_trampoline, name, true);
      return nb_trampoline.base().attr(ticket.key)(std::forward<Args>(args)...);
    });
  }

  callback_cell cell_;
};

thread_local building_state building{};

auto current_build() -> building_state
{
  if(not building.alive)
  {
    throw config_error("models", 0, "python", "a Python model built with its machine from Python");
  }
  return building;
}

auto to_model(
    nb::object const&      made,
    periph::model_context& ctx,
    peripheral_desc const& desc,
    alive_flag             alive) -> std::shared_ptr<periph::model>
{
  std::shared_ptr<periph::model> model;
  if(not nb::try_cast(made, model) or not model)
  {
    throw config_error(
        desc.name, 0, "model",
        std::format(
            "a popcorn.model.Model instance, not {}", nb::cast<std::string>(nb::repr(made))));
  }
  if(auto* const python = dynamic_cast<python_model*>(model.get()))
  {
    python->run_in(building_context(), std::move(alive), building_collected(), ctx.services);
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
  return [callable = held_bare(std::move(maker))](
             periph::model_context& ctx, peripheral_desc const& desc) {
    auto            made  = current_build();
    auto            alive = made.alive;
    gil_entry const gil;
    return to_model(
        callable->fn(model_context_view{ctx, std::move(made.alive), std::move(made.host)}), ctx,
        desc, std::move(alive));
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

building_scope::building_scope(building_state state) noexcept
    : previous_{std::exchange(building, std::move(state))}
{}

building_scope::~building_scope()
{
  building = std::move(previous_);
}

auto building_alive() -> alive_flag
{
  return building.alive;
}

auto building_collected() -> alive_flag
{
  return building.collected;
}

auto building_context() -> nb::object
{
  if(building.context == nullptr)
  {
    return nb::none();
  }
  return nb::borrow(building.context);
}
}

namespace popcorn::bind
{
void register_python_model(std::string const& ip, std::string const& version_glob)
{
  periph::register_model(
      ip, version_glob,
      [ip, version_glob](periph::model_context& ctx, peripheral_desc const& desc) {
        auto                    made = detail::current_build();
        detail::gil_entry const gil;
        auto const makers = reflex::py::nb::module_::import_("popcorn.model").attr("_REGISTERED");
        auto const maker  = makers[reflex::py::nb::make_tuple(ip, version_glob)];
        auto       alive  = made.alive;
        return detail::to_model(
            maker(model_context_view{ctx, std::move(made.alive), std::move(made.host)}), ctx, desc,
            std::move(alive));
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
