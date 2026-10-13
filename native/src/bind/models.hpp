#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/model.hpp>

#include "alive.hpp"
#include "gil_locks.hpp"

#include <reflex/py.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace popcorn::bind
{
namespace py = reflex::py;

/**
 * @brief What a Python model reaches of its machine: register storage, IRQ lines, events, clock
 *        and peers. Valid while the machine lives, every method raises ConfigError after. No
 *        DMA request lines: the STM32 models route DMA requests among themselves through their
 *        peers, the core's model_context::dma stays empty.
 */
class[[= py::rename{"ModelContext"}]] model_context_view
{
public:
  /**
   * @brief Views the context of a model.
   * @param ctx The context the machine built.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, reached by the methods that touch the engine.
   */
  [[= py::skip]] model_context_view(
      periph::model_context& ctx,
      detail::alive_flag     alive,
      detail::host_ref       host) noexcept;

  /** @brief Description of the peripheral. */
  [[= py::doc{"Description of the peripheral."}]] [[nodiscard]] auto description() const
      -> peripheral_desc;

  /** @brief Description of the whole MCU. */
  [[= py::doc{"Description of the whole MCU."}]] [[nodiscard]] auto part() const
      -> part_description;

  /**
   * @brief Stored word of the peripheral, without read effects.
   * @param offset Word-aligned byte offset inside the peripheral.
   * @throws config_error when the offset is not word-aligned or outside the peripheral.
   */
  [[= py::doc{"Stored word at a word-aligned offset, without read effects."}]] [[nodiscard]] auto
      peek(std::uint32_t offset) const -> std::uint32_t;

  /**
   * @brief Stores a word of the peripheral, without write effects.
   * @param offset Word-aligned byte offset inside the peripheral.
   * @param value Stored word.
   * @throws config_error when the offset is not word-aligned or outside the peripheral.
   */
  [[= py::doc{"Store a word at a word-aligned offset, without write effects."}]] void
      poke(std::uint32_t offset, std::uint32_t value);

  /** @brief IRQ lines of the peripheral. */
  [[= py::doc{"IRQ lines of the peripheral."}]] [[nodiscard]] auto irq_count() const
      -> std::uint32_t;

  /**
   * @brief Drives an IRQ line.
   * @param index Line index, in description order.
   * @param level Active or inactive.
   * @throws config_error when the index is out of range.
   */
  [[= py::doc{"Drive an IRQ line active or inactive."}]] void
      set_irq(std::uint32_t index, bool level);

  /**
   * @brief Level of an IRQ line.
   * @param index Line index, in description order.
   * @throws config_error when the index is out of range.
   */
  [[= py::doc{"Whether an IRQ line is active."}]] [[nodiscard]] auto
      irq_level(std::uint32_t index) const -> bool;

  /**
   * @brief Schedules an event of the model at a virtual time, kept when the core clock changes
   *        before it fires.
   * @param delay_ps Virtual time from now, in picoseconds.
   * @param kind Kind passed back to the model.
   * @param payload Payload passed back to the model.
   * @return Handle for cancel().
   * @throws config_error when the due time overflows.
   */
  [[= py::doc{"Schedule an event delay_ps from now, return its handle."}]] auto
      schedule(std::uint64_t delay_ps, std::uint32_t kind, std::uint64_t payload) -> std::uint64_t;

  /**
   * @brief Cancels a pending event of the model, a no-op for a fired or unknown one.
   * @param handle Handle from schedule().
   */
  [[= py::doc{"Cancel a pending event of this model."}]] void cancel(std::uint64_t handle);

  /** @brief Virtual time since reset, in picoseconds. */
  [[= py::doc{"Virtual time since reset, in picoseconds."}]] [[nodiscard]] auto time_ps() const
      -> std::uint64_t;

  /** @brief Instructions executed since reset. */
  [[= py::doc{"Instructions executed since reset."}]] [[nodiscard]] auto instructions() const
      -> std::uint64_t;

  /** @brief Whether the machine runs with strict_peripherals, fixed for the machine's life. */
  [[= py::doc{"Whether the machine runs with strict_peripherals, fixed for the machine's life."}]]
      [[nodiscard]] auto strict_peripherals() const -> bool;

  /**
   * @brief Asks the run to stop at the next block boundary.
   * @param reason Reason the stop reports.
   * @param detail Text of the stop.
   * @throws config_error when the reason is not a stop reason.
   */
  [[= py::doc{"Stop the run at the next block boundary."}]] void
      request_stop(stop_reason reason, std::string detail);

  /**
   * @brief Python model of another peripheral of the machine.
   * @param peripheral Peripheral name.
   * @return The model object, None when the peripheral has no Python model or the machine is
   *         still building its models.
   */
  [[= py::doc{"Python model of another peripheral, None when it has none."}]] [[nodiscard]] auto
      peer(std::string const& peripheral) const -> reflex::py::nb::object;

private:
  [[nodiscard]] auto            context() const -> periph::model_context&;
  template <typename Body> auto engine(Body&& body) const;

  periph::model_context* ctx_;
  detail::alive_flag     alive_;
  detail::host_ref       host_;
};

/**
 * @brief Serves every peripheral of an IP with the Python model registered in `popcorn.model`
 *        for it, in every machine built from now on.
 * @param ip IP name, as the description spells it.
 * @param version_glob Glob over the IP version, `*` matches any.
 */
[[= py::doc{"Route an IP to the Python model popcorn.model registers for it."}]] void
    register_python_model(std::string const& ip, std::string const& version_glob);

/**
 * @brief Removes a registration of register_python_model().
 * @param ip IP name.
 * @param version_glob Glob of the registration.
 */
[[= py::doc{"Remove a registration of register_python_model."}]] void
    unregister_python_model(std::string const& ip, std::string const& version_glob);

/**
 * @brief Glob of the registration, built-in or Python, that serves a peripheral of an IP and IP
 *        version in a machine built now.
 * @param ip IP name.
 * @param ip_version IP version.
 * @return The glob, empty when no registration serves it.
 */
[[= py::doc{"Glob of the registration serving an IP version, or None."}]] [[nodiscard]] auto
    registered_model_glob(std::string const& ip, std::string const& ip_version)
        -> std::optional<std::string>;

namespace detail
{
/** @brief Binds `ModelBase`, the nanobind trampoline over periph::model. */
void bind_model_base(reflex::py::nb::module_ scope);

/**
 * @brief A model factory calling a Python callable with the ModelContext, which returns the
 *        model object.
 * @param maker The callable.
 */
auto python_model_factory(reflex::py::nb::object maker) -> periph::model_factory;

/**
 * @brief The Python object of a model.
 * @param model A model, null allowed.
 * @return The object, None for a native model or null.
 */
auto python_object(periph::model* model) -> reflex::py::nb::object;

/** @brief What the Python models and devices of a machine built on this thread are given. */
struct building_state
{
  /** @brief Cleared when the machine is destroyed. */
  detail::alive_flag alive{};
  /** @brief Set once the machine's `Machine` is found garbage. */
  detail::alive_flag collected{};
  /** @brief The machine's host, set once the machine is handed over. */
  detail::host_ref host{};
  /** @brief Weak reference to the cell whose context the models and devices run in, borrowed for
   * the build, null for none. */
  PyObject* context = nullptr;
};

/**
 * @brief Marks the machine being built on this thread, so its Python models see its lifetime,
 *        its host and the context captured for it.
 */
class building_scope
{
public:
  /**
   * @brief Marks a machine as being built.
   * @param state Its lifetime flag, host slot and context.
   */
  explicit building_scope(building_state state) noexcept;
  /** @brief Not copyable. */
  building_scope(building_scope const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(building_scope const&) -> building_scope& = delete;
  /** @brief Restores the previous mark. */
  ~building_scope();

private:
  building_state previous_;
};

/**
 * @brief The lifetime flag of the machine being built on this thread.
 * @return The flag, null outside a build.
 */
auto building_alive() -> alive_flag;

/**
 * @brief The collected flag of the machine being built on this thread.
 * @return The flag, null outside a build.
 */
auto building_collected() -> alive_flag;

/**
 * @brief The weak reference to the context cell of the machine being built on this thread.
 * @return A new reference, None outside a build or without a cell.
 */
auto building_context() -> reflex::py::nb::object;
}
}
