#include "system.hpp"

#include "chip_select.hpp"
#include "gil_locks.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <reflex/enum.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;

thread_local core::system const* holding = nullptr;

class holding_scope
{
public:
  explicit holding_scope(core::system const& sys) noexcept : previous_{holding}
  {
    holding = &sys;
  }
  holding_scope(holding_scope const&)                    = delete;
  auto operator=(holding_scope const&) -> holding_scope& = delete;
  ~holding_scope()
  {
    holding = previous_;
  }

private:
  core::system const* previous_;
};

void refuse_caller(core::system const& sys)
{
  if(core::detail::on_host_thread)
  {
    throw std::runtime_error("a linked system is not driven from a callback of a machine");
  }
  if(core::detail::host_locks_held != 0)
  {
    throw popcorn::machine_running("a linked system is not driven from inside a parked section");
  }
  if(holding == &sys)
  {
    throw std::runtime_error(
        "a linked system is not driven from a callback that runs while it completes a request");
  }
}

struct closer_registry
{
  std::mutex                                                      mutex;
  std::condition_variable                                         idle;
  std::uint64_t                                                   next{};
  std::map<std::uint64_t, std::vector<std::weak_ptr<core::host>>> live;
};

auto closers() -> closer_registry&
{
  static auto* const registry = new closer_registry;
  return *registry;
}

auto closer_delay() -> std::chrono::milliseconds
{
  if(auto const* given = std::getenv("POPCORN_TEST_CLOSER_DELAY_MS"); given != nullptr)
  {
    std::int64_t value{};
    auto const   text = std::string_view{given};
    if(auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
       error == std::errc{} and end == text.data() + text.size() and value >= 0)
    {
      return std::chrono::milliseconds{value};
    }
  }
  return {};
}

void drop_system(std::shared_ptr<core::system> sys) noexcept
{
  if(sys == nullptr)
  {
    return;
  }
  if(core::detail::on_host_thread or core::detail::host_locks_held != 0 or holding == sys.get())
  {
    std::vector<std::weak_ptr<core::host>> hosts;
    try
    {
      for(auto const& one : sys->hosts())
      {
        hosts.push_back(one);
      }
    }
    catch(...)
    {}
    auto&         registry = closers();
    std::uint64_t id{};
    {
      std::scoped_lock const lock{registry.mutex};
      id = registry.next++;
      registry.live.emplace(id, std::move(hosts));
    }
    try
    {
      std::thread([sys = std::move(sys), id, delay = closer_delay()]() mutable {
        sys->close();
        std::this_thread::sleep_for(delay);
        sys.reset();
        auto& registry = closers();
        {
          std::scoped_lock const lock{registry.mutex};
          registry.live.erase(id);
        }
        registry.idle.notify_all();
      }).detach();
    }
    catch(...)
    {
      {
        std::scoped_lock const lock{registry.mutex};
        registry.live.erase(id);
      }
      std::ignore = new std::shared_ptr<core::system>(std::move(sys));
    }
    return;
  }
  if(PyGILState_Check() != 0)
  {
    nb::gil_scoped_release const released;
    sys.reset();
    return;
  }
  sys.reset();
}

class system_ref
{
public:
  explicit system_ref(std::shared_ptr<core::system> sys) noexcept : sys_{std::move(sys)}
  {}
  system_ref(system_ref const&)                    = delete;
  auto operator=(system_ref const&) -> system_ref& = delete;
  ~system_ref()
  {
    drop_system(std::move(sys_));
  }
  auto operator*() const noexcept -> core::system&
  {
    return *sys_;
  }
  explicit operator bool() const noexcept
  {
    return sys_ != nullptr;
  }

private:
  std::shared_ptr<core::system> sys_;
};

struct goal_chain
{
  std::mutex                               mutex;
  bool                                     fired{};
  std::move_only_function<void() noexcept> next;

  void fire() noexcept
  {
    std::move_only_function<void() noexcept> run;
    {
      std::scoped_lock const lock{mutex};
      fired = true;
      run   = std::move(next);
    }
    if(run)
    {
      run();
    }
  }

  void then(std::move_only_function<void() noexcept> callback)
  {
    {
      std::scoped_lock const lock{mutex};
      if(not fired)
      {
        next = std::move(callback);
        return;
      }
    }
    callback();
  }
};

class goal_request final : public detail::pending_request
{
public:
  goal_request(
      core::host_future<core::goal_reached>                                future,
      std::move_only_function<nb::object(core::goal_reached const&) const> convert,
      std::move_only_function<bool()>                                      cancel,
      std::weak_ptr<detail::host_slot const>                               slot)
      : inner_{future, std::move(convert), std::move(cancel)}
  {
    future.on_done([chain = chain_, slot = std::move(slot)]() noexcept {
      if(auto const owner = slot.lock())
      {
        owner->refused.store(false, std::memory_order_release);
      }
      chain->fire();
    });
  }

  [[nodiscard]] auto done() const -> bool override
  {
    return inner_.done();
  }

  auto wait_for(std::chrono::nanoseconds limit) const -> bool override
  {
    return inner_.wait_for(limit);
  }

  [[nodiscard]] auto value() const -> nb::object override
  {
    return inner_.value();
  }

  void on_done(std::move_only_function<void() noexcept> callback) override
  {
    chain_->then(std::move(callback));
  }

  auto cancel() -> bool override
  {
    return inner_.cancel();
  }

  [[nodiscard]] auto failure() const -> std::pair<std::exception_ptr, std::string> override
  {
    return inner_.failure();
  }

private:
  std::shared_ptr<goal_chain>                chain_ = std::make_shared<goal_chain>();
  detail::future_request<core::goal_reached> inner_;
};

auto take(core::system& sys) -> core::held_system
{
  refuse_caller(sys);
  if(auto held = sys.try_lock(); held.holds(sys))
  {
    return held;
  }
  nb::gil_scoped_release const released;
  return sys.lock();
}

template <typename Body> auto locked(core::system& sys, Body&& body)
{
  auto const                   held = take(sys);
  holding_scope const          scope{sys};
  nb::gil_scoped_release const released;
  return std::invoke(std::forward<Body>(body), held);
}

auto pause_tuple(core::pause const& paused) -> nb::object
{
  nb::object member = nb::none();
  if(paused.member)
  {
    member = nb::cast(*paused.member);
  }
  nb::object error = nb::none();
  if(paused.error)
  {
    error = detail::python_exception(paused.error);
  }
  return nb::make_tuple(
      paused.reason, member, paused.at.ps, detail::stop_info(paused.stopped), error, paused.windows,
      paused.slices);
}

auto request_pause_of(std::weak_ptr<core::system> weak) -> std::move_only_function<bool()>
{
  return [weak = std::move(weak)] {
    system_ref const sys{weak.lock()};
    if(sys)
    {
      nb::gil_scoped_release const released;
      (*sys).request_pause();
    }
    return false;
  };
}

void check_shape(wire_spec const& spec, std::size_t members)
{
  if(spec.sender >= members or spec.receiver >= members)
  {
    throw std::invalid_argument("a wire names a member the system does not have");
  }
  if(spec.latency_ps <= 0)
  {
    throw std::invalid_argument("a wire's latency must be above 0");
  }
  if(spec.baud == 0)
  {
    throw std::invalid_argument("a wire's baud must be above 0");
  }
  if(spec.word_bits < 7 or spec.word_bits > 9)
  {
    throw std::invalid_argument("a wire's data bits must be 7, 8 or 9");
  }
  if(spec.half_stops < 1 or spec.half_stops > 4)
  {
    throw std::invalid_argument("a wire's stop length must be 1 to 4 half bits");
  }
  if(spec.parity > 2)
  {
    throw std::invalid_argument("a wire's parity must be 0, 1 or 2");
  }
}
}

detail::uart_medium::uart_medium(
    std::shared_ptr<periph::stm32::uart_wire> wire,
    std::size_t                               sender,
    host_ref                                  slot,
    periph::uart_port&                        port,
    std::shared_ptr<periph::uart_endpoint>    own)
    : wire_{std::move(wire)}, sender_{sender}, slot_{std::move(slot)}, port_{&port},
      own_{std::move(own)}
{}

auto detail::uart_medium::links() const -> std::vector<std::shared_ptr<core::member_link>>
{
  return {wire_};
}

void detail::uart_medium::attach()
{
  host_access(slot_, [&] { port_->attach(wire_); });
}

void detail::uart_medium::give_back() noexcept
{
  retire(slot_, [weak = std::weak_ptr<host_slot const>{slot_}, port = port_, own = own_] {
    auto const owner = weak.lock();
    if(owner == nullptr)
    {
      return;
    }
    if(auto const on = owner->host.lock(); on != nullptr and not on->abandoned())
    {
      port->attach(own);
    }
  });
}

auto detail::uart_medium::wire() const noexcept -> periph::stm32::uart_wire&
{
  return *wire_;
}

auto detail::uart_medium::sender() const noexcept -> std::size_t
{
  return sender_;
}

auto detail::uart_medium::linked() const noexcept -> bool
{
  return linked_;
}

void detail::uart_medium::link() noexcept
{
  linked_ = true;
}

detail::spi_bus_medium::spi_bus_medium(
    std::shared_ptr<periph::stm32::spi_medium> bus,
    host_ref                                   master,
    std::vector<host_ref>                      slaves)
    : bus_{std::move(bus)}, master_{std::move(master)}, slaves_{std::move(slaves)}
{}

auto detail::spi_bus_medium::links() const -> std::vector<std::shared_ptr<core::member_link>>
{
  return bus_->links();
}

void detail::spi_bus_medium::attach()
{
  host_access(master_, [&] { bus_->attach_master(); });
  for(std::size_t index = 0; index < slaves_.size(); ++index)
  {
    host_access(slaves_[index], [&] { bus_->attach_slave(index); });
  }
}

namespace
{
template <typename Bus, typename Body>
void hand_back(detail::host_ref const& slot, std::shared_ptr<Bus> const& bus, Body body)
{
  detail::retire(
      slot, [weak = std::weak_ptr<detail::host_slot const>{slot}, bus, body = std::move(body)] {
        auto const owner = weak.lock();
        if(owner == nullptr)
        {
          return;
        }
        if(auto const on = owner->host.lock(); on != nullptr and not on->abandoned())
        {
          body(*bus);
        }
      });
}
}

void detail::spi_bus_medium::give_back() noexcept
{
  hand_back(master_, bus_, [](periph::stm32::spi_medium& bus) { bus.give_back_master(); });
  for(std::size_t index = 0; index < slaves_.size(); ++index)
  {
    hand_back(slaves_[index], bus_, [index](periph::stm32::spi_medium& bus) {
      bus.give_back_slave(index);
    });
  }
}

auto detail::spi_bus_medium::bus() const noexcept -> periph::stm32::spi_medium&
{
  return *bus_;
}

detail::i2c_bus_medium::i2c_bus_medium(
    std::shared_ptr<periph::stm32::i2c_medium> bus,
    host_ref                                   controller,
    std::vector<host_ref>                      targets)
    : bus_{std::move(bus)}, controller_{std::move(controller)}, targets_{std::move(targets)}
{}

auto detail::i2c_bus_medium::links() const -> std::vector<std::shared_ptr<core::member_link>>
{
  return bus_->links();
}

void detail::i2c_bus_medium::attach()
{
  host_access(controller_, [&] { bus_->attach_controller(); });
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    host_access(targets_[index], [&] { bus_->attach_target(index); });
  }
}

void detail::i2c_bus_medium::give_back() noexcept
{
  hand_back(controller_, bus_, [](periph::stm32::i2c_medium& bus) { bus.give_back_controller(); });
  for(std::size_t index = 0; index < targets_.size(); ++index)
  {
    hand_back(targets_[index], bus_, [index](periph::stm32::i2c_medium& bus) {
      bus.give_back_target(index);
    });
  }
}

auto detail::i2c_bus_medium::bus() const noexcept -> periph::stm32::i2c_medium&
{
  return *bus_;
}

system_handle::system_handle(std::uint64_t slice_instructions, std::int64_t slice_wall_ns)
{
  if(slice_instructions == 0)
  {
    throw config_error("slice_instructions", 0, "slice_instructions", "above 0");
  }
  if(slice_wall_ns <= 0)
  {
    throw config_error("slice_wall_budget", 0, "slice_wall_budget", "above 0");
  }
  bounds_ = {
      .max_instructions = icount{slice_instructions},
      .wall_budget      = std::chrono::nanoseconds{slice_wall_ns}};
}

system_handle::~system_handle()
{
  closed_ = true;
  for(auto const& one : members_)
  {
    one.slot->refused.store(false, std::memory_order_release);
  }
  auto sys = std::move(system_);
  if(sys == nullptr)
  {
    return;
  }
  if(Py_IsFinalizing() != 0)
  {
    std::ignore = new std::shared_ptr<core::system>(std::move(sys));
    return;
  }
  drop_system(std::move(sys));
  give_back();
}

auto system_handle::add_member(machine_handle& member, std::string name, bool stop_on_reset)
    -> std::size_t
{
  if(system_ != nullptr or closed_)
  {
    throw std::logic_error("members are added before start()");
  }
  members_.push_back(
      {.handle        = &member,
       .name          = std::move(name),
       .stop_on_reset = stop_on_reset,
       .host          = member.host(),
       .slot          = member.slot()});
  handles_.push_back(nb::find(&member));
  return members_.size() - 1;
}

auto system_handle::add_wire(
    wire_spec const&  spec,
    uart_queue const& sender,
    uart_queue const& receiver) -> std::size_t
{
  if(system_ != nullptr or closed_)
  {
    throw std::logic_error("wires are added before start()");
  }
  check_shape(spec, members_.size());
  auto* const into = dynamic_cast<periph::stm32::uart_link_port*>(&receiver.native_port());
  if(into == nullptr)
  {
    throw config_error(spec.receiver_unit, 0, "wire", "a UART model that takes frames from a wire");
  }
  auto const index   = wires_.size();
  auto const lane    = std::exchange(next_lane_, next_lane_ + 2);
  auto*      machine = &members_[spec.receiver].handle->native();
  auto&      port    = sender.native_port();
  auto       wire    = std::make_shared<periph::stm32::uart_wire>(
      periph::stm32::wire_ends{.sender = spec.sender, .receiver = spec.receiver},
      periph::stm32::wire_lanes{.peer = lane, .injected = lane + 1},
      core::sys_time{spec.latency_ps},
      periph::stm32::line_shape{
          .baud       = spec.baud,
          .word_bits  = spec.word_bits,
          .half_stops = spec.half_stops,
          .parity     = static_cast<periph::line_parity>(spec.parity)},
      periph::stm32::wire_receiver{
          .port   = into,
          .unit   = spec.receiver_unit,
          .log    = [machine](core::log_record record) { machine->log(std::move(record)); },
          .strict = spec.strict},
      sender.endpoint(), tap_capacity_);
  auto& added = media_.emplace_back(
      std::in_place_type<detail::uart_medium>, std::move(wire), spec.sender,
      members_[spec.sender].slot, port, sender.endpoint());
  wires_.push_back(static_cast<detail::uart_medium*>(&*added));
  return index;
}

auto system_handle::add_link(std::size_t forth, std::size_t back) -> std::size_t
{
  if(system_ != nullptr or closed_)
  {
    throw std::logic_error("links are added before start()");
  }
  if(forth >= wires_.size() or back >= wires_.size())
  {
    throw std::invalid_argument("a link names a wire the system does not have");
  }
  auto& one   = *wires_[forth];
  auto& other = *wires_[back];
  if(forth == back
     or one.sender() != other.wire().receiver()
     or other.sender() != one.wire().receiver())
  {
    throw std::invalid_argument("a link pairs the two directions between two members");
  }
  if(one.linked() or other.linked())
  {
    throw std::invalid_argument("a wire is in a link already");
  }
  auto registration = std::make_shared<periph::stm32::link_registration>();
  one.wire().share_registration(registration);
  other.wire().share_registration(std::move(registration));
  one.link();
  other.link();
  links_.emplace_back(forth, back, one.sender());
  return links_.size() - 1;
}

namespace
{
template <typename Port> auto link_port_of(core::machine& target, std::string const& unit) -> Port*
{
  auto const& peripherals = target.description().peripherals;
  auto const  named       = std::ranges::find(peripherals, unit, &peripheral_desc::name);
  if(named == peripherals.end())
  {
    return nullptr;
  }
  return dynamic_cast<Port*>(target.model(named->alias_of.value_or(unit)));
}

template <typename Port>
auto link_end_of(machine_handle& member, std::string const& unit, std::string key, char const* kind)
    -> Port&
{
  auto* const port = link_port_of<Port>(member.native(), unit);
  if(port == nullptr)
  {
    throw config_error(unit, 0, std::move(key), std::format("an {} instance of the member", kind));
  }
  return *port;
}

auto spi_end_of(machine_handle& member, std::string const& unit, std::string key)
    -> periph::stm32::spi_link_port&
{
  return link_end_of<periph::stm32::spi_link_port>(member, unit, std::move(key), "SPI");
}

auto i2c_end_of(machine_handle& member, std::string const& unit, std::string key)
    -> periph::stm32::i2c_link_port&
{
  return link_end_of<periph::stm32::i2c_link_port>(member, unit, std::move(key), "I2C");
}

void check_chip_select(
    machine_handle&    master,
    std::string const& port,
    std::uint8_t       pin,
    std::string        key)
{
  auto&       target = master.native();
  auto* const gpio   = [&]() -> periph::stm32::gpio_port* {
    auto const& peripherals = target.description().peripherals;
    if(std::ranges::find(peripherals, port, &peripheral_desc::name) == peripherals.end())
    {
      return nullptr;
    }
    return dynamic_cast<periph::stm32::gpio_port*>(target.model(port));
  }();
  if(gpio == nullptr)
  {
    throw config_error(port, 0, std::move(key), "a GPIO port of the master's part");
  }
  if(pin >= gpio->pin_count())
  {
    throw config_error(port, 0, "pin", periph::stm32::pin_range_text(gpio->pin_count()));
  }
  detail::check_bonded_chip_select(target.description(), port, pin);
}
}

auto system_handle::add_spi_bus(spi_bus_spec const& spec) -> std::size_t
{
  if(system_ != nullptr or closed_)
  {
    throw std::logic_error("buses are added before start()");
  }
  if(spec.master >= members_.size() or spec.slaves.empty() or spec.latency_ps <= 0)
  {
    throw std::invalid_argument("a bus needs a master member, a slave and a latency above 0");
  }
  for(auto const& slave : spec.slaves)
  {
    if(slave.member >= members_.size() or slave.member == spec.master)
    {
      throw std::invalid_argument("a slave is a member other than the master");
    }
  }
  auto& master      = *members_[spec.master].handle;
  auto& master_port = spi_end_of(master, spec.master_unit, "master");
  if(host_access(members_[spec.master].slot, [&] { return master_port.board_use().controller; }))
  {
    throw config_error(spec.master_unit, 0, "master", "an SPI instance with no board controller");
  }
  std::vector<periph::stm32::spi_slave_end> ends;
  std::vector<detail::host_ref>             slots;
  for(std::size_t index = 0; index < spec.slaves.size(); ++index)
  {
    auto const& slave = spec.slaves[index];
    auto const  key   = std::format("slaves[{}]", index);
    if(slave.cs_pin)
    {
      check_chip_select(master, slave.cs_port, *slave.cs_pin, key);
    }
    auto&      port = spi_end_of(*members_[slave.member].handle, slave.unit, key);
    auto const use  = host_access(members_[slave.member].slot, [&] { return port.board_use(); });
    if(use.controller or use.devices)
    {
      throw config_error(
          slave.unit, 0, key, "an SPI instance with no board controller and no board device");
    }
    ends.push_back(
        {.member     = slave.member,
         .port       = &port,
         .cs_port    = slave.cs_port,
         .cs_pin     = slave.cs_pin,
         .forth_lane = next_lane_,
         .back_lane  = next_lane_ + 1});
    next_lane_ += 2;
    slots.push_back(members_[slave.member].slot);
  }
  auto bus = std::make_shared<periph::stm32::spi_medium>(
      spec.master, master_port, core::sys_time{spec.latency_ps}, std::move(ends), tap_capacity_);
  auto& added = media_.emplace_back(
      std::in_place_type<detail::spi_bus_medium>, std::move(bus), members_[spec.master].slot,
      std::move(slots));
  buses_.push_back(static_cast<detail::spi_bus_medium*>(&*added));
  return buses_.size() - 1;
}

auto system_handle::spi_tap_take(std::size_t bus) -> std::vector<spi_tap_record>
{
  if(bus >= buses_.size())
  {
    throw std::out_of_range("no such SPI bus");
  }
  std::vector<spi_tap_record> out;
  for(auto const& frame : buses_[bus]->bus().tap().take())
  {
    out.push_back(
        {.slave    = frame.slave,
         .unit     = frame.unit,
         .cs_pin   = frame.cs_pin,
         .at_ps    = frame.at.ps,
         .reply_ps = frame.reply.ps,
         .mosi     = frame.mosi,
         .miso     = frame.miso,
         .bits     = frame.bits,
         .dropped  = frame.dropped});
  }
  return out;
}

auto system_handle::spi_tap_dropped(std::size_t bus) -> std::uint64_t
{
  if(bus >= buses_.size())
  {
    throw std::out_of_range("no such SPI bus");
  }
  return buses_[bus]->bus().tap().dropped();
}

auto system_handle::add_i2c_bus(i2c_bus_spec const& spec) -> std::size_t
{
  if(system_ != nullptr or closed_)
  {
    throw std::logic_error("buses are added before start()");
  }
  if(spec.controller >= members_.size() or spec.targets.empty() or spec.latency_ps <= 0)
  {
    throw std::invalid_argument("a bus needs a controller member, a target and a latency above 0");
  }
  for(auto const& target : spec.targets)
  {
    if(target.member >= members_.size() or target.member == spec.controller)
    {
      throw std::invalid_argument("a target is a member other than the controller");
    }
  }
  auto& controller_port =
      i2c_end_of(*members_[spec.controller].handle, spec.controller_unit, "controller");
  if(host_access(
         members_[spec.controller].slot, [&] { return controller_port.board_use().controller; }))
  {
    throw config_error(
        spec.controller_unit, 0, "controller", "an I2C instance with no board controller");
  }
  std::vector<periph::stm32::i2c_target_end> ends;
  std::vector<detail::host_ref>              slots;
  for(std::size_t index = 0; index < spec.targets.size(); ++index)
  {
    auto const& target = spec.targets[index];
    auto const  key    = std::format("targets[{}]", index);
    auto&       port   = i2c_end_of(*members_[target.member].handle, target.unit, key);
    auto const  use = host_access(members_[target.member].slot, [&] { return port.board_use(); });
    if(use.controller or use.targets)
    {
      throw config_error(
          target.unit, 0, key, "an I2C instance with no board controller and no board target");
    }
    ends.push_back(
        {.member     = target.member,
         .port       = &port,
         .forth_lane = next_lane_,
         .back_lane  = next_lane_ + 1});
    next_lane_ += 2;
    slots.push_back(members_[target.member].slot);
  }
  auto bus = std::make_shared<periph::stm32::i2c_medium>(
      spec.controller, controller_port, core::sys_time{spec.latency_ps}, std::move(ends),
      tap_capacity_);
  auto& added = media_.emplace_back(
      std::in_place_type<detail::i2c_bus_medium>, std::move(bus), members_[spec.controller].slot,
      std::move(slots));
  i2c_buses_.push_back(static_cast<detail::i2c_bus_medium*>(&*added));
  return i2c_buses_.size() - 1;
}

auto system_handle::i2c_tap_take(std::size_t bus) -> std::vector<i2c_tap_record>
{
  if(bus >= i2c_buses_.size())
  {
    throw std::out_of_range("no such I2C bus");
  }
  std::vector<i2c_tap_record> out;
  for(auto const& phase : i2c_buses_[bus]->bus().tap().take())
  {
    out.push_back(
        {.target    = phase.target,
         .unit      = phase.unit,
         .phase     = static_cast<std::uint8_t>(phase.phase),
         .byte      = phase.byte,
         .ack       = phase.ack,
         .at_ps     = phase.at.ps,
         .answer_ps = phase.answer.ps,
         .dropped   = phase.dropped});
  }
  return out;
}

auto system_handle::i2c_tap_dropped(std::size_t bus) -> std::uint64_t
{
  if(bus >= i2c_buses_.size())
  {
    throw std::out_of_range("no such I2C bus");
  }
  return i2c_buses_[bus]->bus().tap().dropped();
}

void system_handle::start(std::vector<std::int64_t> const& offsets_ps)
{
  if(system_ != nullptr or closed_)
  {
    throw std::logic_error("the system was started already");
  }
  if(members_.empty() or media_.empty())
  {
    throw std::logic_error("a linked system needs a member and a wire");
  }
  if(offsets_ps.size() != members_.size())
  {
    throw std::logic_error("one offset per member");
  }
  std::vector<core::member_ref> refs;
  for(auto const& one : members_)
  {
    refs.push_back(
        {.on       = one.host,
         .name     = one.name,
         .on_reset = one.stop_on_reset ? core::reset_request_policy::stop
                                       : core::reset_request_policy::reset_and_continue});
  }
  std::vector<std::shared_ptr<core::member_link>> links;
  try
  {
    for(auto& one : media_)
    {
      one->attach();
      std::ranges::copy(one->links(), std::back_inserter(links));
    }
    system_ = [&] {
      nb::gil_scoped_release const released;
      return std::make_shared<core::system>(
          std::move(refs), std::move(links), bounds_, std::move_only_function<void(std::size_t)>{},
          [sleeps = sleeps_](std::size_t) { sleeps->fetch_add(1, std::memory_order_relaxed); });
    }();
  }
  catch(...)
  {
    give_back();
    throw;
  }
  std::vector<core::sys_time> offsets;
  for(auto const one : offsets_ps)
  {
    offsets.push_back(core::sys_time{one});
  }
  locked(*system_, [&](core::held_system const& held) { system_->start(held, offsets); });
}

auto system_handle::system() const -> std::shared_ptr<core::system>
{
  if(closed_)
  {
    throw core::machine_closed("the linked system was closed");
  }
  if(system_ == nullptr)
  {
    throw std::logic_error("the linked system is not started");
  }
  refuse_caller(*system_);
  return system_;
}

void system_handle::check_caller() const
{
  system_ref const ref{system()};
}

auto system_handle::member_index(std::size_t member) const -> std::size_t
{
  if(member >= members_.size())
  {
    throw std::out_of_range("no such member");
  }
  return member;
}

auto system_handle::wire(std::size_t index) const -> periph::stm32::uart_wire&
{
  if(index >= wires_.size())
  {
    throw std::out_of_range("no such wire");
  }
  return wires_[index]->wire();
}

auto system_handle::link(std::size_t index) const -> link_entry const&
{
  if(index >= links_.size())
  {
    throw std::out_of_range("no such link");
  }
  return links_[index];
}

auto system_handle::native() const noexcept -> core::system*
{
  return system_.get();
}

auto system_handle::resume(std::optional<std::int64_t> until_ps) -> host_future_handle
{
  system_ref const ref{system()};
  auto&            sys    = *ref;
  auto             future = locked(sys, [&](core::held_system const& held) {
    std::optional<core::sys_time> until;
    if(until_ps)
    {
      until = core::sys_time{*until_ps};
    }
    return sys.resume(held, until);
  });
  return host_future_handle{std::make_shared<detail::future_request<core::pause>>(
      std::move(future), pause_tuple, request_pause_of(system_))};
}

auto system_handle::post_goal(
    std::size_t                                       member,
    std::optional<address>                            until,
    std::optional<std::uint64_t>                      budget,
    std::optional<std::uint64_t>                      exact,
    std::optional<std::int64_t>                       until_ps,
    std::optional<nb::object>                         stop_when,
    std::optional<native_predicate>                   gate,
    bool                                              open,
    std::optional<std::int64_t>                       cap_ps,
    std::optional<std::tuple<address, std::uint64_t>> until_return) -> host_future_handle
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  std::ignore          = member_index(member);
  core::member_goal goal{.until = until, .open = open};
  if(until_return)
  {
    auto const [ret, sp] = *until_return;
    goal.until_return    = core::frame_return{.ret = ret, .sp = sp};
  }
  if(budget)
  {
    goal.budget = icount{*budget};
  }
  if(exact)
  {
    goal.exact = icount{*exact};
  }
  if(until_ps)
  {
    goal.until_time = core::sys_time{*until_ps};
  }
  if(cap_ps)
  {
    goal.cap = core::sys_time{*cap_ps};
  }
  std::shared_ptr<detail::python_callable> kept;
  if(stop_when and not stop_when->is_none())
  {
    kept           = detail::held(std::move(*stop_when));
    goal.stop_when = [predicate = kept] {
      detail::gil_entry const gil;
      return nb::cast<bool>(detail::call_python([&] { return (*predicate)(); }));
    };
  }
  if(gate)
  {
    goal.stop_when = [test = gate->test(), then = std::move(goal.stop_when)] mutable {
      return test() and (not then or then());
    };
  }
  auto const& slot   = members_[member].slot;
  auto        future = locked(sys, [&](core::held_system const& held) {
    auto posted = sys.post_goal(held, member, std::move(goal));
    slot->refused.store(true, std::memory_order_release);
    return posted;
  });
  return host_future_handle{
      std::make_shared<goal_request>(
          std::move(future),
          [kept](core::goal_reached const& reached) {
            return nb::make_tuple(detail::stop_info(reached.stopped), reached.at.ps);
          },
          request_pause_of(system_), std::weak_ptr<detail::host_slot const>{slot}),
      slot};
}

auto system_handle::post_waits(
    std::size_t                                  member,
    std::vector<address>                         untils,
    std::vector<address>                         arrive_at,
    std::optional<std::int64_t>                  until_ps,
    std::vector<std::optional<native_predicate>> gates,
    std::vector<std::optional<nb::object>>       stop_whens,
    std::optional<std::int64_t>                  cap_ps) -> host_future_handle
{
  if(gates.size() != stop_whens.size() or gates.size() > 64)
  {
    throw nb::value_error("post_waits takes at most 64 waits, as many gates as stop_whens");
  }
  system_ref const ref{system()};
  auto&            sys = *ref;
  std::ignore          = member_index(member);
  core::member_goal goal{.arrive_at = std::move(arrive_at), .open = true};
  if(not untils.empty())
  {
    goal.until = untils.front();
    goal.until_also.assign(std::next(untils.begin()), untils.end());
  }
  if(until_ps)
  {
    goal.until_time = core::sys_time{*until_ps};
  }
  if(cap_ps)
  {
    goal.cap = core::sys_time{*cap_ps};
  }
  struct wait_test
  {
    std::move_only_function<bool() const>    gate;
    std::shared_ptr<detail::python_callable> python;
  };
  auto tests  = std::make_shared<std::vector<wait_test>>();
  auto asking = false;
  for(std::size_t index = 0; index < gates.size(); ++index)
  {
    auto& test = tests->emplace_back();
    if(gates[index])
    {
      test.gate = gates[index]->test();
    }
    if(stop_whens[index] and not stop_whens[index]->is_none())
    {
      test.python = detail::held(std::move(*stop_whens[index]));
    }
    asking = asking or test.gate or test.python;
  }
  auto held = std::make_shared<std::atomic<std::uint64_t>>(0);
  if(asking)
  {
    goal.stop_when = [tests, held] {
      std::uint64_t mask = 0;
      for(std::size_t index = 0; index < tests->size(); ++index)
      {
        auto const& test = (*tests)[index];
        if(not test.gate and not test.python)
        {
          continue;
        }
        if(test.gate and not test.gate())
        {
          continue;
        }
        if(test.python)
        {
          detail::gil_entry const gil;
          if(not nb::cast<bool>(detail::call_python([&] { return (*test.python)(); })))
          {
            continue;
          }
        }
        mask |= std::uint64_t{1} << index;
      }
      held->store(mask, std::memory_order_relaxed);
      return mask != 0;
    };
  }
  auto const& slot   = members_[member].slot;
  auto        future = locked(sys, [&](core::held_system const& held_system) {
    auto posted = sys.post_goal(held_system, member, std::move(goal));
    slot->refused.store(true, std::memory_order_release);
    return posted;
  });
  return host_future_handle{
      std::make_shared<goal_request>(
          std::move(future),
          [tests, held](core::goal_reached const& reached) {
            return nb::make_tuple(
                detail::stop_info(reached.stopped), reached.at.ps,
                held->load(std::memory_order_relaxed));
          },
          request_pause_of(system_), std::weak_ptr<detail::host_slot const>{slot}),
      slot};
}

void system_handle::withdraw(std::size_t member)
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  locked(sys, [&](core::held_system const& held) { sys.withdraw(held, member_index(member)); });
}

void system_handle::release(std::size_t member, bool halt)
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  locked(
      sys, [&](core::held_system const& held) { sys.release(held, member_index(member), halt); });
}

auto system_handle::undecided() -> std::vector<std::size_t>
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  return locked(sys, [&](core::held_system const& held) { return sys.undecided(held); });
}

void system_handle::finish()
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  locked(sys, [&](core::held_system const& held) { sys.finish(held); });
}

void system_handle::request_pause()
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  refuse_caller(sys);
  nb::gil_scoped_release const released;
  sys.request_pause();
}

void system_handle::wake()
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  refuse_caller(sys);
  nb::gil_scoped_release const released;
  sys.wake();
}

void system_handle::reset(std::size_t member, reset_kind kind)
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  locked(sys, [&](core::held_system const& held) { sys.reset(held, member_index(member), kind); });
}

void system_handle::close()
{
  if(closed_)
  {
    return;
  }
  if(system_ != nullptr)
  {
    refuse_caller(*system_);
  }
  closed_ = true;
  for(auto const& one : members_)
  {
    one.slot->refused.store(false, std::memory_order_release);
  }
  if(auto sys = std::move(system_))
  {
    {
      nb::gil_scoped_release const released;
      sys->close();
    }
    drop_system(std::move(sys));
  }
  give_back();
}

void system_handle::give_back() noexcept
{
  for(auto& one : media_)
  {
    one->give_back();
  }
}

auto system_handle::save() -> nb::bytes
{
  system_ref const       ref{system()};
  auto&                  sys = *ref;
  std::vector<std::byte> out;
  locked(sys, [&](core::held_system const& held) { sys.save(held, out); });
  return nb::bytes(reinterpret_cast<char const*>(out.data()), out.size());
}

void system_handle::load(nb::bytes const& state)
{
  system_ref const ref{system()};
  auto&            sys   = *ref;
  auto const       bytes = std::span{static_cast<std::byte const*>(state.data()), state.size()};
  std::vector<std::byte> const copy{bytes.begin(), bytes.end()};
  locked(sys, [&](core::held_system const& held) { sys.load(held, copy); });
}

void system_handle::check_state(nb::bytes const& state)
{
  system_ref const ref{system()};
  auto&            sys   = *ref;
  auto const       bytes = std::span{static_cast<std::byte const*>(state.data()), state.size()};
  std::vector<std::byte> const copy{bytes.begin(), bytes.end()};
  locked(sys, [&](core::held_system const& held) { sys.check_state(held, copy); });
}

auto system_handle::quantum_ps() const -> std::int64_t
{
  system_ref const ref{system()};
  return (*ref).quantum().ps;
}

auto system_handle::pending_barrier_ps() -> std::int64_t
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  return locked(sys, [&](core::held_system const& held) { return sys.pending_barrier(held).ps; });
}

auto system_handle::time_of_ps(std::size_t member) -> std::int64_t
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  std::ignore          = member_index(member);
  return locked(sys, [&](core::held_system const& held) { return sys.time_of(held, member).ps; });
}

auto system_handle::base_of_ps(std::size_t member) -> std::int64_t
{
  if(closed_)
  {
    throw core::machine_closed("the linked system was closed");
  }
  if(system_ == nullptr)
  {
    throw std::logic_error("the linked system is not started");
  }
  std::ignore = member_index(member);
  return system_->base_of(member).ps;
}

auto system_handle::executed_of(std::size_t member) -> std::uint64_t
{
  system_ref const ref{system()};
  auto&            sys = *ref;
  std::ignore          = member_index(member);
  return locked(
      sys, [&](core::held_system const& held) { return sys.executed_of(held, member).value; });
}

auto system_handle::sleeps() const noexcept -> std::uint64_t
{
  return sleeps_->load(std::memory_order_relaxed);
}

auto system_handle::tap_take(std::size_t index) -> std::vector<tap_record>
{
  std::vector<tap_record> out;
  for(auto const& frame : wire(index).tap().take())
  {
    out.push_back(
        {.from       = frame.from,
         .to         = frame.to,
         .index      = frame.index,
         .data       = frame.data,
         .start_ps   = frame.start.ps,
         .end_ps     = frame.end.ps,
         .arrival_ps = frame.arrival.ps,
         .brk        = frame.brk,
         .lost       = frame.lost,
         .dropped    = frame.dropped,
         .injected   = frame.injected,
         .cut        = frame.cut});
  }
  return out;
}

auto system_handle::tap_dropped(std::size_t index) -> std::uint64_t
{
  return wire(index).tap().dropped();
}

void system_handle::set_tap_capacity(std::size_t capacity)
{
  if(capacity == 0)
  {
    throw std::invalid_argument("a tap keeps at least 1 frame");
  }
  tap_capacity_ = capacity;
}

auto system_handle::inject(
    std::size_t                       index,
    std::vector<std::uint16_t> const& data,
    std::int64_t                      at_ps,
    bool paused) -> std::optional<std::tuple<std::string, std::int64_t>>
{
  if(data.empty())
  {
    throw std::invalid_argument("inject takes at least one frame");
  }
  system_ref const                                   ref{system()};
  auto&                                              sys = *ref;
  auto&                                              one = wire(index);
  auto const                                         at  = core::sys_time{at_ps};
  std::expected<void, periph::stm32::inject_refusal> outcome;
  if(paused)
  {
    auto const& member = members_[one.receiver()];
    outcome            = locked(sys, [&](core::held_system const& held) {
      auto const              now  = sys.time_of(held, one.receiver());
      auto const              lock = member.host->lock();
      core::owner_scope const owning{*member.host, lock};
      auto const local = member.host->member(lock, core::member_id{}).virtual_clock().time();
      periph::stm32::stopped_receiver const stopped{
          .now = now,
          .map = core::time_map{core::sys_time{now.ps - static_cast<std::int64_t>(local.ps)}}};
      return one.inject(data, at, stopped);
    });
  }
  else
  {
    refuse_caller(sys);
    nb::gil_scoped_release const released;
    outcome = one.inject(data, at, std::nullopt);
    if(outcome)
    {
      sys.wake();
    }
  }
  if(outcome)
  {
    return std::nullopt;
  }
  return std::tuple{std::format("{}", outcome.error().reason), outcome.error().bound.ps};
}

auto system_handle::change_cut(
    std::size_t      index,
    std::string_view what,
    bool             stopped,
    bool             from_body,
    cut_change       change) -> cut_outcome
{
  auto const& entry = link(index);
  auto&       forth = wire(entry.forth);
  auto&       back  = wire(entry.back);
  auto const  owner = entry.owner.load(std::memory_order_relaxed);
  auto const  refused =
      [](std::expected<core::sys_time, periph::stm32::cut_refusal> const& outcome) -> cut_outcome {
    if(outcome)
    {
      return {outcome->ps, std::nullopt};
    }
    return {
        std::nullopt, std::tuple{
                                 std::format("{}", outcome.error().reason), outcome.error().bound.ps,
                                 outcome.error().end.ps}
    };
  };
  if(core::detail::host_locks_held != 0)
  {
    throw popcorn::machine_running("a linked system is not driven from inside a parked section");
  }
  if(core::detail::on_host_thread)
  {
    if(closed_)
    {
      throw core::machine_closed("the linked system was closed");
    }
    if(system_ == nullptr)
    {
      throw std::logic_error("the linked system is not started");
    }
    auto const caller = std::ranges::find_if(
        members_, [](member_entry const& one) { return one.host->owns_engine(); });
    if(caller == members_.end())
    {
      throw std::runtime_error(
          std::format(
              "{} from a callback of a machine outside this system: only callbacks of {}, the "
              "link's cut owner, change its cuts",
              what, members_[owner].name));
    }
    if(static_cast<std::size_t>(caller - members_.begin()) != owner)
    {
      throw std::runtime_error(
          std::format(
              "{} from a callback: only callbacks of {}, the link's cut owner, change its cuts",
              what, members_[owner].name));
    }
    auto const local = caller->host->member_inline(core::member_id{}).virtual_clock().time();
    auto const now   = system_->base_of(owner).ps + static_cast<std::int64_t>(local.ps);
    return refused(change(forth, back, core::sys_time{now}));
  }
  system_ref const ref{system()};
  auto&            sys = *ref;
  if(stopped)
  {
    return refused(locked(sys, [&](core::held_system const& held) {
      auto const now = std::max(
          sys.time_of(held, wires_[entry.forth]->sender()),
          sys.time_of(held, wires_[entry.back]->sender()));
      return change(forth, back, now);
    }));
  }
  if(not from_body)
  {
    throw std::runtime_error(
        std::format(
            "{} while the system runs: call it from a callback of {}, the link's cut owner, "
            "or call cut(start, end) from here",
            what, members_[owner].name));
  }
  refuse_caller(sys);
  nb::gil_scoped_release const released;
  auto const                   outcome = change(forth, back, std::nullopt);
  if(outcome)
  {
    sys.wake();
  }
  return refused(outcome);
}

auto system_handle::cut(
    std::size_t  index,
    std::int64_t start_ps,
    std::int64_t end_ps,
    bool         low,
    bool         stopped) -> cut_outcome
{
  if(end_ps <= start_ps)
  {
    throw std::invalid_argument("a cut ends after its start");
  }
  return change_cut(
      index, "cut", stopped, true,
      [&](periph::stm32::uart_wire& forth, periph::stm32::uart_wire& back,
          std::optional<core::sys_time> now) {
        return periph::stm32::cut_link(
            forth, back, core::sys_time{start_ps}, core::sys_time{end_ps}, low, now);
      });
}

auto system_handle::break_line(std::size_t index, bool low, bool stopped) -> cut_outcome
{
  return change_cut(
      index, "break_line", stopped, false,
      [&](periph::stm32::uart_wire& forth, periph::stm32::uart_wire& back,
          std::optional<core::sys_time> now) {
        return periph::stm32::cut_link(forth, back, *now, std::nullopt, low, now);
      });
}

auto system_handle::restore(std::size_t index, bool stopped) -> cut_outcome
{
  return change_cut(
      index, "restore_line", stopped, false,
      [](periph::stm32::uart_wire& forth, periph::stm32::uart_wire& back,
         std::optional<core::sys_time> now) {
        return periph::stm32::restore_link(forth, back, *now);
      });
}

auto system_handle::link_owner(std::size_t index) const -> std::size_t
{
  auto const& entry = link(index);
  if(closed_)
  {
    throw core::machine_closed("the linked system was closed");
  }
  return entry.owner.load(std::memory_order_relaxed);
}

auto system_handle::max_waiting_cuts() const -> std::size_t
{
  return periph::stm32::max_waiting_cuts;
}

void system_handle::set_link_owner(std::size_t index, std::size_t member)
{
  std::ignore = link(index);
  links_[index].owner.store(member_index(member), std::memory_order_relaxed);
}

auto system_handle::replace(
    std::size_t                       index,
    std::uint64_t                     first,
    std::vector<std::uint16_t> const& data) -> bool
{
  if(data.empty())
  {
    throw std::invalid_argument("replace takes at least one frame");
  }
  return wire(index).replace(first, data);
}

auto system_handle::drop(std::size_t index, std::uint64_t first, std::uint64_t count) -> bool
{
  if(count == 0)
  {
    throw std::invalid_argument("drop takes at least one frame");
  }
  return wire(index).drop(first, count);
}

auto system_handle::posted(std::size_t index) -> std::uint64_t
{
  return wire(index).posted();
}

void detail::settle_closers(std::chrono::steady_clock::time_point until)
{
  auto&      registry    = closers();
  auto const empty       = [&registry] { return registry.live.empty(); };
  auto       interrupted = false;
  while(not interrupted and std::chrono::steady_clock::now() < until)
  {
    auto settled = false;
    {
      nb::gil_scoped_release const released;
      std::unique_lock             lock{registry.mutex};
      settled = registry.idle.wait_for(lock, std::chrono::milliseconds{50}, empty);
    }
    if(settled)
    {
      return;
    }
    if(PyErr_CheckSignals() != 0)
    {
      PyErr_Clear();
      interrupted = true;
    }
  }
  std::vector<std::shared_ptr<core::host>> stuck;
  {
    std::scoped_lock const lock{registry.mutex};
    for(auto const& [id, hosts] : registry.live)
    {
      for(auto const& weak : hosts)
      {
        if(auto on = weak.lock())
        {
          stuck.push_back(std::move(on));
        }
      }
    }
    if(registry.live.empty())
    {
      return;
    }
  }
  PySys_WriteStderr(
      "popcorn: a linked system closed by a machine's thread did not finish at exit, the process "
      "exits without joining its members\n");
  nb::gil_scoped_release const released;
  for(auto const& on : stuck)
  {
    on->abandon_thread();
  }
}

auto system_handle::shape() const -> std::tuple<std::size_t, std::size_t, bool>
{
  return {members_.size(), wires_.size(), system_ != nullptr};
}
}
