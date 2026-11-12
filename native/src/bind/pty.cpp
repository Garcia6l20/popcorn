#include "pty.hpp"

#include <fcntl.h>
#include <pty.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
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
  path_          = found.data();
  clients_watch_ = unique_fd{::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
  if(clients_watch_.get() < 0
     or ::inotify_add_watch(clients_watch_.get(), path_.c_str(), IN_OPEN | IN_CLOSE) < 0)
  {
    auto const cause = std::generic_category().message(errno);
    clients_watch_   = unique_fd{};
    clients_warning_ = std::format(
        "{}: cannot watch {} for closes ({}), a tool's TIOCEXCL stays set until the device "
        "detaches or attaches",
        name_, path_, cause);
  }
}

auto pty_bridge::take_sessions() -> std::vector<session_edge>
{
  std::vector<session_edge> changes;
  if(clients_watch_.get() < 0)
  {
    return changes;
  }
  alignas(inotify_event) std::array<char, 4096> buffer{};
  bool                                          closed  = false;
  bool                                          opened  = false;
  bool                                          recount = false;
  for(;;)
  {
    auto const got = ::read(clients_watch_.get(), buffer.data(), buffer.size());
    if(got < 0 and errno == EINTR)
    {
      continue;
    }
    if(got <= 0)
    {
      break;
    }
    for(std::size_t at = 0; at + sizeof(inotify_event) <= static_cast<std::size_t>(got);)
    {
      inotify_event event{};
      std::memcpy(&event, buffer.data() + at, sizeof event);
      at += sizeof event + event.len;
      if((event.mask & IN_Q_OVERFLOW) != 0)
      {
        recount = true;
      }
      else if((event.mask & IN_OPEN) != 0)
      {
        opened = true;
      }
      else if((event.mask & IN_CLOSE) != 0)
      {
        closed = true;
      }
    }
  }
  if(not link_->wants_sessions())
  {
    int exclusive = 0;
    if(closed
       and ::ioctl(slave_.get(), TIOCGEXCL, &exclusive) == 0
       and exclusive != 0
       and walk_holders(true).empty())
    {
      (void)::ioctl(slave_.get(), TIOCNXCL);
    }
    return changes;
  }
  if(opened and not recount and not held_)
  {
    held_ = true;
    changes.push_back(session_edge::opened);
  }
  if(not closed and not recount)
  {
    return changes;
  }
  auto const holds = still_held() or not(holders_ = walk_holders(false)).empty();
  if(holds != held_ and (recount or not holds))
  {
    held_ = holds;
    changes.push_back(holds ? session_edge::opened : session_edge::closed);
  }
  if(not holds)
  {
    (void)::ioctl(slave_.get(), TIOCNXCL);
  }
  return changes;
}

void pty_bridge::deliver(
    std::vector<session_edge> const& sessions,
    std::vector<std::uint8_t> const& input) const
{
  auto const trailing_close = not sessions.empty() and sessions.back() == session_edge::closed;
  auto const before         = sessions.size() - (trailing_close ? 1 : 0);
  for(auto const edge : sessions | std::views::take(before))
  {
    if(edge == session_edge::opened)
    {
      link_->host_opened();
    }
    else
    {
      link_->host_closed();
    }
  }
  if(not input.empty())
  {
    link_->send(input);
  }
  if(trailing_close)
  {
    link_->host_closed();
  }
  if(sessions.empty() and link_->unsettled())
  {
    link_->settle();
  }
}

auto pty_bridge::still_held() const -> bool
{
  struct stat node{};
  if(holders_.empty() or ::stat(path_.c_str(), &node) != 0)
  {
    return false;
  }
  return std::ranges::any_of(holders_, [&](std::string const& fd) {
    std::error_code gone;
    struct stat     seen{};
    return std::filesystem::read_symlink(fd, gone) == path_
       and not gone
       and ::stat(fd.c_str(), &seen) == 0
       and seen.st_dev == node.st_dev
       and seen.st_ino == node.st_ino;
  });
}

auto pty_bridge::walk_holders(bool first_only) -> std::vector<std::string>
{
  namespace fs = std::filesystem;
  ++scans_;
  std::vector<std::string> found;
  auto const               own_slave = std::to_string(slave_.get());
  auto const               skip      = fs::directory_options::skip_permission_denied;
  std::error_code          failed;
  struct stat              node{};
  if(::stat(path_.c_str(), &node) != 0)
  {
    return found;
  }
  for(fs::directory_iterator process{"/proc", skip, failed};
      not failed and process != fs::directory_iterator{}; process.increment(failed))
  {
    auto const pid = process->path().filename().string();
    if(pid.empty() or not std::ranges::all_of(pid, [](char c) { return c >= '0' and c <= '9'; }))
    {
      continue;
    }
    std::error_code unreadable;
    for(fs::directory_iterator fd{process->path() / "fd", skip, unreadable};
        not unreadable and fd != fs::directory_iterator{}; fd.increment(unreadable))
    {
      std::error_code gone;
      struct stat     seen{};
      if(fs::read_symlink(fd->path(), gone) != path_
         or gone
         or ::stat(fd->path().c_str(), &seen) != 0
         or seen.st_dev != node.st_dev
         or seen.st_ino != node.st_ino)
      {
        continue;
      }
      if(fd->path().filename().string() == own_slave
         and fs::equivalent(process->path(), "/proc/self", gone))
      {
        continue;
      }
      found.push_back(fd->path().string());
      if(first_only)
      {
        return found;
      }
    }
  }
  return found;
}

auto pty_bridge::pump() -> std::optional<std::string>
{
  auto const sessions = take_sessions();
  deliver(sessions, take_input());
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
  if(link_->device_changed())
  {
    (void)::ioctl(slave_.get(), TIOCNXCL);
  }
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
  if(not warning and not clients_warning_.empty())
  {
    warning = std::exchange(clients_warning_, std::string{});
  }
  return warning;
}

void pty_bridge::discard() noexcept
{
  outgoing_.clear();
  wants_write_->store(false, std::memory_order_relaxed);
  link_->discard();
}

auto pump_all(pty_bridges& ptys) -> std::vector<pty_warning>
{
  struct arrival
  {
    std::shared_ptr<pty_bridge> bridge;
    std::vector<session_edge>   sessions;
    std::vector<std::uint8_t>   input;
  };
  std::vector<arrival> arrivals;
  {
    std::scoped_lock const lock{ptys.mutex};
    for(auto const& bridge : ptys.bridges)
    {
      auto sessions = bridge->take_sessions();
      auto incoming = bridge->take_input();
      if(not sessions.empty() or not incoming.empty() or bridge->settling())
      {
        arrivals.push_back({bridge, std::move(sessions), std::move(incoming)});
      }
    }
  }
  for(auto const& [bridge, sessions, incoming] : arrivals)
  {
    bridge->deliver(sessions, incoming);
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
    periph::uart_port&              port,
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

cdc_pty_link::cdc_pty_link(
    std::shared_ptr<periph::usb::cdc_hub>     hub,
    std::size_t                               function,
    std::uint64_t                             attachments,
    std::move_only_function<bool(bool, bool)> post) noexcept
    : hub_{std::move(hub)}, function_{function}, seen_{attachments}, post_{std::move(post)}
{}

auto cdc_pty_link::view() -> line_view
{
  auto const  held    = hub_->lock();
  auto const& channel = hub_->channel(held, function_);
  if(requested_ and requested_at_ == channel.attachments)
  {
    return {channel.ready, *requested_};
  }
  requested_.reset();
  return {channel.ready, channel.line_state};
}

void cdc_pty_link::request(std::uint8_t lines)
{
  if(post_((lines & 1U) != 0, (lines & 2U) != 0))
  {
    auto const held = hub_->lock();
    requested_      = lines;
    requested_at_   = hub_->channel(held, function_).attachments;
  }
}

void cdc_pty_link::raise()
{
  auto const state = view();
  unsettled_       = not state.ready;
  if(state.ready and state.lines != 3U)
  {
    request(3U);
  }
}

void cdc_pty_link::settle()
{
  if(tool_holds_)
  {
    raise();
  }
  else
  {
    unsettled_ = false;
  }
}

void cdc_pty_link::host_opened()
{
  tool_holds_ = true;
  raise();
}

void cdc_pty_link::host_closed()
{
  tool_holds_      = false;
  unsettled_       = false;
  auto const state = view();
  if(state.ready and (state.lines & 1U) != 0)
  {
    request(0U);
  }
}

void cdc_pty_link::discard() noexcept
{
  requested_.reset();
  unsettled_ = tool_holds_;
}

void cdc_pty_link::send(std::span<std::uint8_t const> bytes)
{
  auto const held = hub_->lock();
  hub_->channel(held, function_).out.append_range(bytes);
}

auto cdc_pty_link::device_changed() -> bool
{
  auto const held     = hub_->lock();
  auto const attached = hub_->channel(held, function_).attachments;
  auto const changed  = std::exchange(seen_, attached) != attached;
  unsettled_          = unsettled_ or (changed and tool_holds_);
  return changed;
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
