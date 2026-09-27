#include "board.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <vector>

namespace popcorn::bind
{
uart_link::uart_link(periph::stm32::uart_port& port, std::shared_ptr<bool const> alive)
    : port_{&port}, endpoint_{std::make_shared<detail::queue_endpoint>()}, alive_{std::move(alive)}
{
  port.attach(endpoint_);
}

auto uart_link::port() const -> periph::stm32::uart_port&
{
  if(not *alive_)
  {
    throw config_error("uart", 0, "machine", "a UART whose machine still exists");
  }
  return *port_;
}

void uart_link::send(reflex::py::nb::bytes const& data)
{
  auto const bytes = std::span{static_cast<std::uint8_t const*>(data.data()), data.size()};
  std::vector<std::uint16_t> const frames(bytes.begin(), bytes.end());
  port().receive(frames);
}

auto uart_link::queued() const -> std::uint64_t
{
  return port().queued();
}

auto uart_link::available() const noexcept -> std::uint64_t
{
  return endpoint_->frames.size();
}

auto uart_link::drain(std::uint64_t count) -> std::vector<uart_frame>
{
  auto const taken = static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(count, available()));
  std::vector<uart_frame> out(endpoint_->frames.begin(), endpoint_->frames.begin() + taken);
  endpoint_->frames.erase(endpoint_->frames.begin(), endpoint_->frames.begin() + taken);
  return out;
}

auto machine_handle::attach_uart(std::string const& peripheral) -> uart_link
{
  auto* const port = dynamic_cast<periph::stm32::uart_port*>(machine_->model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  return uart_link{*port, alive_};
}

auto machine_handle::gpio(std::string const& peripheral, std::uint8_t pin) const
    -> periph::stm32::gpio_port&
{
  auto* const port = dynamic_cast<periph::stm32::gpio_port*>(machine_->model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  if(pin > 15)
  {
    throw config_error(peripheral, 0, "pin", std::format("a pin from 0 to 15, not {}", pin));
  }
  return *port;
}

void machine_handle::gpio_drive(
    std::string const&  peripheral,
    std::uint8_t        pin,
    std::optional<bool> level)
{
  gpio(peripheral, pin).drive(pin, level);
}

auto machine_handle::gpio_level(std::string const& peripheral, std::uint8_t pin) const -> bool
{
  return gpio(peripheral, pin).level(pin);
}

auto machine_handle::gpio_mode(std::string const& peripheral, std::uint8_t pin) const
    -> periph::stm32::pin_mode
{
  return gpio(peripheral, pin).mode(pin);
}

auto machine_handle::gpio_alternate(std::string const& peripheral, std::uint8_t pin) const
    -> std::uint8_t
{
  return gpio(peripheral, pin).alternate(pin);
}
}
