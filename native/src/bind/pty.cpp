#include "pty.hpp"

#include <popcorn/core/host.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <linux/kcmp.h>
#include <poll.h>
#include <pty.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <ranges>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace popcorn::bind::detail
{
namespace
{
[[noreturn]] void fail(char const* what)
{
  throw std::system_error(errno, std::generic_category(), what);
}

auto each_name(
    int                                       directory,
    std::function_ref<bool()>                 interrupted,
    std::function_ref<bool(std::string_view)> visit) -> bool
{
  alignas(dirent64) std::array<char, 2048> buffer{};
  for(;;)
  {
    if(interrupted())
    {
      return false;
    }
    auto const got = ::getdents64(directory, buffer.data(), buffer.size());
    if(got <= 0)
    {
      return true;
    }
    for(std::size_t at = 0; at < static_cast<std::size_t>(got);)
    {
      unsigned short length = 0;
      std::memcpy(&length, buffer.data() + at + offsetof(dirent64, d_reclen), sizeof length);
      std::string_view const name{buffer.data() + at + offsetof(dirent64, d_name)};
      at += length;
      if(name != "." and name != ".." and not visit(name))
      {
        return false;
      }
    }
  }
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

auto start_thread(std::move_only_function<void()> body) -> std::jthread
{
  return std::jthread{std::move(body)};
}

pty_bridge::pty_bridge(std::string name, std::unique_ptr<pty_link> link, thread_starter start)
    : name_{std::move(name)}, link_{std::move(link)}, wants_sessions_{link_->wants_sessions()},
      forks_{(core::host::watch_forks(), core::host::fork_generation())}
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
    return;
  }
  try
  {
    stop_ = unique_fd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    if(stop_.get() < 0)
    {
      fail("eventfd");
    }
    if(wants_sessions_)
    {
      wake_ = unique_fd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
      if(wake_.get() < 0)
      {
        fail("eventfd");
      }
    }
    watcher_ = start([this] { watch(); });
  }
  catch(std::system_error const& refused)
  {
    stop_            = unique_fd{};
    wake_            = unique_fd{};
    clients_warning_ = std::format(
        "{}: cannot start the thread watching {} ({}), a tool's TIOCEXCL is cleared at the next "
        "slice after its close",
        name_, path_, refused.code().message());
  }
}

pty_bridge::~pty_bridge()
{
  if(not watcher_.joinable())
  {
    return;
  }
  if(forks_ != core::host::fork_generation())
  {
    std::ignore = std::make_unique<std::jthread>(std::move(watcher_)).release();
    return;
  }
  (void)::eventfd_write(stop_.get(), 1);
  watcher_.join();
}

void pty_bridge::watch() noexcept
{
  std::array<pollfd, 2> polled{
      {
       {.fd = clients_watch_.get(), .events = POLLIN, .revents = 0},
       {.fd = stop_.get(), .events = POLLIN, .revents = 0},
       }
  };
  auto unverified = false;
  for(;;)
  {
    auto const ready = ::poll(polled.data(), polled.size(), unverified ? pty_quiet_ms : -1);
    if(ready < 0)
    {
      if(errno != EINTR)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      continue;
    }
    if(polled[1].revents != 0)
    {
      return;
    }
    auto changed = false;
    {
      std::scoped_lock const lock{clients_mutex_};
      auto const             quiet = ready == 0;
      changed                      = consume(lock, quiet ? verify::when_quiet : verify::later);
      unverified                   = clients_.unverified;
      changed = changed or (quiet and not unverified and (clients_.closed or clients_.overflowed));
    }
    if(changed and wake_.get() >= 0)
    {
      (void)::eventfd_write(wake_.get(), 1);
    }
  }
}

auto pty_bridge::read_events(std::scoped_lock<std::mutex> const&) -> batch
{
  alignas(inotify_event) std::array<char, 4096> buffer{};
  batch                                         read;
  for(;;)
  {
    auto const got = ::read(clients_watch_.get(), buffer.data(), buffer.size());
    if(got < 0 and errno == EINTR)
    {
      continue;
    }
    if(got <= 0)
    {
      return read;
    }
    for(std::size_t at = 0; at + sizeof(inotify_event) <= static_cast<std::size_t>(got);)
    {
      inotify_event event{};
      std::memcpy(&event, buffer.data() + at, sizeof event);
      at += sizeof event + event.len;
      read.any = true;
      if((event.mask & IN_Q_OVERFLOW) != 0)
      {
        clients_.overflowed = true;
        clients_.unverified = true;
        clients_.inexact    = true;
      }
      else if((event.mask & IN_OPEN) != 0)
      {
        ++clients_.count;
        clients_.holds  = true;
        clients_.opened = true;
        clients_.joined = clients_.cleared;
      }
      else if((event.mask & IN_CLOSE) != 0)
      {
        read.closed = true;
        clients_.count -= clients_.count > 0 ? 1 : 0;
        clients_.closed     = true;
        clients_.unverified = true;
      }
    }
  }
}

auto pty_bridge::consume(std::scoped_lock<std::mutex> const& held, verify when) -> bool
{
  auto changed = false;
  auto walks   = 0;
  for(;;)
  {
    auto const read = read_events(held);
    changed         = changed or read.any;
    auto exclusive  = 0;
    if(read.closed
       and not clients_.inexact
       and clients_.count == 0
       and ::ioctl(slave_.get(), TIOCGEXCL, &exclusive) == 0
       and exclusive != 0)
    {
      (void)::ioctl(slave_.get(), TIOCNXCL);
      clients_.cleared = true;
      clients_.joined  = false;
    }
    if(when == verify::later or not clients_.unverified)
    {
      return changed;
    }
    exclusive = 0;
    (void)::ioctl(slave_.get(), TIOCGEXCL, &exclusive);
    if(exclusive != 0 or clients_.cleared or wants_sessions_)
    {
      if(not still_held())
      {
        auto found = walk_holders();
        if(not found)
        {
          if(when == verify::when_quiet or ++walks == pty_walk_retries)
          {
            when = verify::later;
          }
          continue;
        }
        holders_ = std::move(*found);
      }
      clients_.holds = not holders_.empty();
      if(not clients_.holds)
      {
        clients_.count   = 0;
        clients_.inexact = false;
        (void)::ioctl(slave_.get(), TIOCNXCL);
      }
      else if(clients_.cleared and not clients_.joined)
      {
        (void)::ioctl(slave_.get(), TIOCEXCL);
      }
    }
    clients_.unverified = false;
    clients_.cleared    = false;
    clients_.joined     = false;
    return changed;
  }
}

auto pty_bridge::take_sessions() -> std::vector<session_edge>
{
  std::vector<session_edge> changes;
  if(clients_watch_.get() < 0)
  {
    return changes;
  }
  if(wake_.get() >= 0)
  {
    eventfd_t drained = 0;
    (void)::eventfd_read(wake_.get(), &drained);
  }
  clients seen;
  {
    std::scoped_lock const lock{clients_mutex_};
    std::ignore     = consume(lock, verify::now);
    seen            = clients_;
    clients_.opened = false;
    if(not seen.unverified)
    {
      clients_.closed = clients_.overflowed = false;
    }
  }
  if(not wants_sessions_)
  {
    return changes;
  }
  if(seen.opened and not seen.overflowed and not held_)
  {
    held_ = true;
    changes.push_back(session_edge::opened);
  }
  if(seen.unverified or (not seen.closed and not seen.overflowed))
  {
    return changes;
  }
  if(seen.holds != held_ and (seen.overflowed or not seen.holds))
  {
    held_ = seen.holds;
    changes.push_back(seen.holds ? session_edge::opened : session_edge::closed);
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

auto pty_bridge::walk_holders() -> std::optional<std::vector<std::string>>
{
  scans_.fetch_add(1, std::memory_order_relaxed);
  std::vector<std::string> found;
  struct stat              node{};
  if(::stat(path_.c_str(), &node) != 0)
  {
    return found;
  }
  auto const self        = ::getpid();
  auto const interrupted = [&] {
    pollfd waiting{.fd = clients_watch_.get(), .events = POLLIN, .revents = 0};
    return ::poll(&waiting, 1, 0) > 0;
  };
  auto const holds = [&](pid_t pid, int fds, std::string_view name) {
    std::array<char, PATH_MAX> target{};
    struct stat                seen{};
    auto                       held = -1;
    auto const length = ::readlinkat(fds, std::string{name}.c_str(), target.data(), target.size());
    std::from_chars(name.data(), name.data() + name.size(), held);
    return length > 0
       and std::string_view{target.data(), static_cast<std::size_t>(length)} == path_
       and ::fstatat(fds, std::string{name}.c_str(), &seen, 0) == 0
       and seen.st_dev == node.st_dev
       and seen.st_ino == node.st_ino
       and not(pid == self and held == slave_.get())
       and ::syscall(SYS_kcmp, self, pid, KCMP_FILE, slave_.get(), held) != 0;
  };
  unique_fd const processes{::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if(processes.get() < 0)
  {
    return found;
  }
  auto const complete = each_name(processes.get(), interrupted, [&](std::string_view name) {
    auto pid = pid_t{};
    if(std::from_chars(name.data(), name.data() + name.size(), pid).ptr
       != name.data() + name.size())
    {
      return true;
    }
    auto const      at = std::format("{}/fd", name);
    unique_fd const fds{::openat(processes.get(), at.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    return fds.get() < 0
        or each_name(
               fds.get(), interrupted,
               [&](std::string_view fd) {
                 if(holds(pid, fds.get(), fd))
                 {
                   found.push_back(std::format("/proc/{}/fd/{}", pid, fd));
                 }
                 return true;
               });
  });
  if(not complete)
  {
    return std::nullopt;
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
    std::scoped_lock const lock{clients_mutex_};
    (void)::ioctl(slave_.get(), TIOCNXCL);
    clients_.cleared = false;
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
