#include "board.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <string>
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

auto uart_link::dropped() const noexcept -> std::uint64_t
{
  return endpoint_->dropped;
}

void uart_link::set_frame_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("uart", 0, "limit", "a frame limit of at least 1");
  }
  endpoint_->limit = limit;
  while(endpoint_->frames.size() > limit)
  {
    endpoint_->frames.pop_front();
    ++endpoint_->dropped;
  }
}

namespace detail
{
void audio_queue_endpoint::transmitted(
    std::span<std::uint32_t const> slots,
    periph::stm32::audio_format    format,
    vtime                          at)
{
  while(starts.size() >= limit)
  {
    pop_front();
    ++dropped;
  }
  auto const count = static_cast<std::uint8_t>(slots.size());
  if(runs.empty() or runs.back().format != format or runs.back().slots != count)
  {
    runs.push_back({.format = format, .slots = count, .frames = 0});
  }
  ++runs.back().frames;
  words.insert(words.end(), slots.begin(), slots.end());
  starts.push_back(at.ps);
}

void audio_queue_endpoint::receive(
    std::span<std::uint32_t> slots,
    periph::stm32::audio_format,
    vtime)
{
  for(auto& slot : slots)
  {
    if(pending.empty())
    {
      slot = 0;
      continue;
    }
    slot = pending.front();
    pending.pop_front();
  }
}

void audio_queue_endpoint::pop_front()
{
  auto& run = runs.front();
  words.erase(words.begin(), words.begin() + run.slots);
  starts.pop_front();
  if(--run.frames == 0)
  {
    runs.pop_front();
  }
}

auto audio_queue_endpoint::take(std::uint64_t count) -> std::vector<audio_record>
{
  std::vector<audio_record> out;
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, starts.size())));
  while(out.size() < count and not starts.empty())
  {
    auto const& run = runs.front();
    out.push_back({
        .slots        = {words.begin(), words.begin() + run.slots},
        .data_bits    = run.format.data_bits,
        .active_slots = run.format.active_slots,
        .mono         = run.format.mono,
        .at_ps        = starts.front(),
    });
    pop_front();
  }
  return out;
}
}

audio_link::audio_link(
    periph::stm32::audio_port&  port,
    periph::stm32::audio_block  block,
    std::uint32_t               decimation,
    std::uint32_t               external_frame_hz,
    bool                        clock_wired,
    std::shared_ptr<bool const> alive)
    : endpoint_{std::make_shared<detail::audio_queue_endpoint>()}, port_{&port}, block_{block},
      alive_{std::move(alive)}
{
  port.attach(
      block, {.endpoint          = endpoint_,
              .decimation        = decimation,
              .external_frame_hz = external_frame_hz,
              .clock_wired       = clock_wired});
}

auto audio_link::endpoint() const -> detail::audio_queue_endpoint&
{
  if(not *alive_)
  {
    throw config_error("audio", 0, "machine", "an SAI whose machine still exists");
  }
  return *endpoint_;
}

auto audio_link::port() const -> periph::stm32::audio_port&
{
  if(not *alive_)
  {
    throw config_error("audio", 0, "machine", "an SAI whose machine still exists");
  }
  return *port_;
}

void audio_link::set_external_frame_hz(std::uint32_t frame_hz)
{
  port().set_external_clock(block_, frame_hz);
}

auto audio_link::external_frame_hz() const -> std::uint32_t
{
  return port().external_clock(block_);
}

auto audio_link::available() const -> std::uint64_t
{
  return endpoint().starts.size();
}

auto audio_link::dropped() const -> std::uint64_t
{
  return endpoint().dropped;
}

auto audio_link::queued() const -> std::uint64_t
{
  return endpoint().pending.size();
}

auto audio_link::drain(std::uint64_t count) -> std::vector<audio_record>
{
  return endpoint().take(count);
}

void audio_link::write(std::vector<std::uint32_t> const& words)
{
  auto& queue = endpoint();
  queue.pending.insert(queue.pending.end(), words.begin(), words.end());
}

void audio_link::discard()
{
  auto& queue = endpoint();
  queue.words.clear();
  queue.starts.clear();
  queue.runs.clear();
  queue.pending.clear();
  queue.dropped = 0;
}

void audio_link::set_frame_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("audio", 0, "limit", "a frame limit of at least 1");
  }
  auto& queue = endpoint();
  queue.limit = limit;
  while(queue.starts.size() > limit)
  {
    queue.pop_front();
    ++queue.dropped;
  }
}

auto machine_handle::audio_names() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const& unit : machine_->description().peripherals)
  {
    if(not unit.alias_of
       and dynamic_cast<periph::stm32::audio_port*>(machine_->model(unit.name)) != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::attach_audio(
    std::string const& peripheral,
    std::string const& block,
    std::uint32_t      decimation,
    std::uint32_t      external_frame_hz,
    bool               clock_wired) -> audio_link
{
  auto* const port = dynamic_cast<periph::stm32::audio_port*>(machine_->model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  if(block != "A" and block != "B")
  {
    throw config_error(peripheral, 0, "block", std::format("an audio block A or B, not {}", block));
  }
  auto const which = block == "A" ? periph::stm32::audio_block::a : periph::stm32::audio_block::b;
  return audio_link{*port, which, decimation, external_frame_hz, clock_wired, alive_};
}

auto machine_handle::uart_names() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const& unit : machine_->description().peripherals)
  {
    if(not unit.alias_of
       and dynamic_cast<periph::stm32::uart_port*>(machine_->model(unit.name)) != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
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

auto machine_handle::analog(std::string const& peripheral) const -> periph::stm32::analog_port&
{
  auto* const port = dynamic_cast<periph::stm32::analog_port*>(machine_->model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  return *port;
}

void machine_handle::adc_drive(
    std::string const& peripheral,
    std::uint8_t       channel,
    std::uint32_t      microvolts)
{
  analog(peripheral).drive(channel, microvolts);
}

auto machine_handle::adc_input(std::string const& peripheral, std::uint8_t channel) const
    -> std::uint32_t
{
  return analog(peripheral).input(channel);
}

auto machine_handle::gpio_alternate(std::string const& peripheral, std::uint8_t pin) const
    -> std::uint8_t
{
  return gpio(peripheral, pin).alternate(pin);
}
}
