#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <format>

namespace popcorn::core
{
machine_services::~machine_services() = default;

void machine_services::do_hold_in_reset(std::string_view peripheral, bool)
{
  throw config_error(
      "machine_services", 0, std::format("hold_in_reset of {}", peripheral),
      "an implementation with peripherals");
}

void machine_services::do_watch_reads(address at, std::uint32_t size, bool)
{
  throw config_error(
      "machine_services", 0, std::format("watch_reads at {:#010x}, {} bytes", at, size),
      "an implementation with controller regions");
}

void machine_services::do_install_firewall(periph::bus_firewall&)
{
  throw config_error(
      "machine_services", 0, "install_firewall", "an implementation with a bus firewall");
}

void machine_services::do_memory_policy_changed()
{}

auto machine_services::do_read_granted(
    address       addr,
    std::uint8_t  size,
    bus_attrs     attrs,
    memory_grant& grant) -> std::expected<std::uint32_t, bus_error>
{
  grant = {};
  return do_read(addr, size, attrs);
}

auto machine_services::do_write_granted(
    address       addr,
    std::uint8_t  size,
    std::uint32_t value,
    bus_attrs     attrs,
    memory_grant& grant) -> std::expected<void, bus_error>
{
  grant = {};
  return do_write(addr, size, value, attrs);
}

auto machine_services::do_emulated_instruction(address) -> emulated_arrival
{
  return emulated_arrival::proceed;
}
}
