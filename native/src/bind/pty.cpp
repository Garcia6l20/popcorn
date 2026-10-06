#include "pty.hpp"

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>

namespace popcorn::bind::detail
{
namespace
{
[[noreturn]] void fail(char const* what)
{
  throw std::system_error(errno, std::generic_category(), what);
}
}

unique_fd::unique_fd(unique_fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)}
{}

auto unique_fd::operator=(unique_fd&& other) noexcept -> unique_fd&
{
  if(this != &other)
  {
    unique_fd const dropped{std::exchange(fd_, std::exchange(other.fd_, -1))};
  }
  return *this;
}

unique_fd::~unique_fd()
{
  if(fd_ >= 0)
  {
    ::close(fd_);
  }
}

pty_bridge::pty_bridge(std::string name, std::unique_ptr<pty_link> link)
    : name_{std::move(name)}, link_{std::move(link)}
{
  int master = -1;
  int slave  = -1;
  if(::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0)
  {
    fail("openpty");
  }
  master_ = unique_fd{master};
  slave_  = unique_fd{slave};
  for(auto const fd : {master, slave})
  {
    if(::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
    {
      fail("fcntl");
    }
  }
  termios mode{};
  if(::tcgetattr(slave, &mode) != 0)
  {
    fail("tcgetattr");
  }
  ::cfmakeraw(&mode);
  if(::tcsetattr(slave, TCSANOW, &mode) != 0)
  {
    fail("tcsetattr");
  }
  auto const flags = ::fcntl(master, F_GETFL);
  if(flags < 0 or ::fcntl(master, F_SETFL, flags | O_NONBLOCK) != 0)
  {
    fail("fcntl");
  }
  std::array<char, PATH_MAX> found{};
  if(auto const failed = ::ttyname_r(slave, found.data(), found.size()); failed != 0)
  {
    errno = failed;
    fail("ttyname_r");
  }
  path_ = found.data();
}

auto pty_bridge::pump() -> std::optional<std::string>
{
  if(auto const incoming = take_input(); not incoming.empty())
  {
    link_->send(incoming);
  }
  return flush();
}

auto pty_bridge::take_input() -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t>      incoming;
  std::array<std::uint8_t, 4096> chunk{};
  for(;;)
  {
    auto const got = ::read(master_.get(), chunk.data(), chunk.size());
    if(got > 0)
    {
      incoming.append_range(std::span{chunk}.first(static_cast<std::size_t>(got)));
    }
    else if(got == 0 or errno != EINTR)
    {
      break;
    }
  }
  return incoming;
}

auto pty_bridge::flush() -> std::optional<std::string>
{
  link_->drain(outgoing_);
  std::size_t written = 0;
  while(written < outgoing_.size())
  {
    auto const put = ::write(master_.get(), outgoing_.data() + written, outgoing_.size() - written);
    if(put > 0)
    {
      written += static_cast<std::size_t>(put);
    }
    else if(put == 0 or errno != EINTR)
    {
      break;
    }
  }
  outgoing_.erase(outgoing_.begin(), outgoing_.begin() + static_cast<std::ptrdiff_t>(written));
  std::optional<std::string> warning;
  if(outgoing_.size() > pty_outgoing_limit)
  {
    outgoing_.erase(
        outgoing_.begin(), outgoing_.end() - static_cast<std::ptrdiff_t>(pty_outgoing_limit));
    if(not std::exchange(overflowed_, true))
    {
      warning = std::format(
          "{}: no host tool reads {}, bytes beyond the last {} were dropped", name_, path_,
          pty_outgoing_limit);
    }
  }
  wants_write_->store(not outgoing_.empty(), std::memory_order_relaxed);
  return warning;
}

void pty_bridge::discard() noexcept
{
  outgoing_.clear();
  wants_write_->store(false, std::memory_order_relaxed);
}

auto pump_all(pty_bridges& ptys) -> std::vector<pty_warning>
{
  std::vector<std::pair<std::shared_ptr<pty_link>, std::vector<std::uint8_t>>> inputs;
  {
    std::scoped_lock const lock{ptys.mutex};
    for(auto const& bridge : ptys.bridges)
    {
      if(auto incoming = bridge->take_input(); not incoming.empty())
      {
        inputs.emplace_back(bridge->link(), std::move(incoming));
      }
    }
  }
  for(auto const& [link, incoming] : inputs)
  {
    link->send(incoming);
  }
  std::vector<pty_warning> warnings;
  {
    std::scoped_lock const lock{ptys.mutex};
    for(auto const& bridge : ptys.bridges)
    {
      if(auto warned = bridge->flush())
      {
        warnings.push_back({.bridge = bridge->name(), .message = std::move(*warned)});
      }
    }
  }
  return warnings;
}

void pump_and_log(pty_bridges& ptys, core::machine& engine)
{
  for(auto& warned : pump_all(ptys))
  {
    engine.log(
        {.kind       = core::log_kind::model_warning,
         .peripheral = std::move(warned.bridge),
         .message    = std::move(warned.message)});
  }
}

void add_bridge(
    pty_bridges& ptys,
    std::unique_lock<std::mutex> const&,
    std::shared_ptr<pty_bridge> bridge)
{
  ptys.bridges.push_back(std::move(bridge));
  ptys.any.store(true, std::memory_order_release);
}

auto remove_bridge(pty_bridges& ptys, std::unique_lock<std::mutex> const&, pty_bridge const* bridge)
    -> std::shared_ptr<pty_bridge>
{
  auto const found = std::ranges::find(ptys.bridges, bridge, &std::shared_ptr<pty_bridge>::get);
  if(found == ptys.bridges.end())
  {
    return {};
  }
  auto removed = std::move(*found);
  ptys.bridges.erase(found);
  ptys.any.store(not ptys.bridges.empty(), std::memory_order_release);
  return removed;
}

uart_pty_link::uart_pty_link(
    periph::stm32::uart_port&       port,
    std::shared_ptr<queue_endpoint> endpoint) noexcept
    : port_{&port}, endpoint_{std::move(endpoint)}
{}

void uart_pty_link::send(std::span<std::uint8_t const> bytes)
{
  std::vector<std::uint16_t> const frames(bytes.begin(), bytes.end());
  port_->receive(frames);
}

void uart_pty_link::drain(std::vector<std::uint8_t>& out)
{
  auto const held  = endpoint_->lock();
  auto const taken = endpoint_->take(held, endpoint_->available(held));
  out.append_range(taken | std::views::transform([](queued_uart_frame const& frame) {
                     return static_cast<std::uint8_t>(frame.data & 0xFFU);
                   }));
}

cdc_pty_link::cdc_pty_link(std::shared_ptr<periph::usb::cdc_hub> hub, std::size_t function) noexcept
    : hub_{std::move(hub)}, function_{function}
{}

void cdc_pty_link::send(std::span<std::uint8_t const> bytes)
{
  auto const held = hub_->lock();
  hub_->channel(held, function_).out.append_range(bytes);
}

void cdc_pty_link::drain(std::vector<std::uint8_t>& out)
{
  auto const held  = hub_->lock();
  auto&      queue = hub_->channel(held, function_);
  for(auto const& packet : queue.take(queue.in_bytes))
  {
    out.append_range(packet.data);
  }
}
}
