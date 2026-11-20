#include "pty.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using popcorn::bind::detail::pty_bridge;
using popcorn::bind::detail::pty_outgoing_limit;

struct fake_link final : popcorn::bind::detail::pty_link
{
  std::vector<std::uint8_t>* pending;
  std::vector<std::uint8_t>* received;

  fake_link(std::vector<std::uint8_t>& out, std::vector<std::uint8_t>& in)
      : pending{&out}, received{&in}
  {}

  void send(std::span<std::uint8_t const> bytes) override
  {
    received->append_range(bytes);
  }

  void drain(std::vector<std::uint8_t>& out) override
  {
    out.append_range(*pending);
    pending->clear();
  }
};

struct session_link final : popcorn::bind::detail::pty_link
{
  std::vector<std::string>* events;

  explicit session_link(std::vector<std::string>& log) : events{&log}
  {}

  void send(std::span<std::uint8_t const> bytes) override
  {
    events->push_back("send " + std::string{bytes.begin(), bytes.end()});
  }

  void drain(std::vector<std::uint8_t>&) override
  {}

  [[nodiscard]] auto wants_sessions() const -> bool override
  {
    return true;
  }

  void host_opened() override
  {
    events->push_back("open");
  }

  void host_closed() override
  {
    events->push_back("close");
  }
};

auto pattern(std::size_t size) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> bytes(size);
  for(std::size_t at = 0; at < size; ++at)
  {
    bytes[at] = static_cast<std::uint8_t>((at * 7) + (at >> 8));
  }
  return bytes;
}

auto open_client(std::string const& path) -> int
{
  auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
  REQUIRE(fd >= 0);
  return fd;
}

void release(std::string const& path, int fd)
{
  auto const watch = ::inotify_init1(IN_CLOEXEC);
  REQUIRE(watch >= 0);
  REQUIRE(::inotify_add_watch(watch, path.c_str(), IN_CLOSE) >= 0);
  ::close(fd);
  pollfd closed{.fd = watch, .events = POLLIN, .revents = 0};
  CHECK(::poll(&closed, 1, 2000) == 1);
  ::close(watch);
}

auto open_error(std::string const& path) -> int
{
  auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
  if(fd >= 0)
  {
    release(path, fd);
    return 0;
  }
  return errno;
}

auto read_ready(int fd, int wait_ms) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> got;
  pollfd                    polled{.fd = fd, .events = POLLIN, .revents = 0};
  if(::poll(&polled, 1, wait_ms) > 0 and (polled.revents & POLLIN) != 0)
  {
    std::array<std::uint8_t, 65536> chunk{};
    auto const                      size = ::read(fd, chunk.data(), chunk.size());
    if(size > 0)
    {
      got.assign(chunk.begin(), chunk.begin() + size);
    }
  }
  return got;
}

auto holds_pty(std::string const& path) -> bool
{
  auto const index = "tty-index:\t" + path.substr(std::string_view{"/dev/pts/"}.size());
  for(auto const& entry : std::filesystem::directory_iterator{"/proc/self/fd"})
  {
    std::error_code   failed;
    auto const        target = std::filesystem::read_symlink(entry.path(), failed).string();
    std::string const deleted{" (deleted)"};
    auto const        bare =
        target.ends_with(deleted) ? target.substr(0, target.size() - deleted.size()) : target;
    if(failed)
    {
      continue;
    }
    if(bare == path)
    {
      return true;
    }
    if(target == "/dev/ptmx" or target == "/dev/pts/ptmx")
    {
      std::ifstream info{"/proc/self/fdinfo/" + entry.path().filename().string()};
      for(std::string line; std::getline(info, line);)
      {
        if(line == index)
        {
          return true;
        }
      }
    }
  }
  return false;
}
}

TEST_CASE("unread output keeps the last 64 KiB and warns once")
{
  auto const                sent = pattern(128 * 1024);
  std::vector<std::uint8_t> pending(sent.begin(), sent.begin() + (sent.size() / 2));
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART9", std::make_unique<fake_link>(pending, received)};
  std::vector<std::string>  warnings;
  if(auto warned = bridge.pump())
  {
    warnings.push_back(*warned);
  }
  pending.assign(sent.begin() + (sent.size() / 2), sent.end());
  if(auto warned = bridge.pump())
  {
    warnings.push_back(*warned);
  }
  REQUIRE(warnings.size() == 1);
  CHECK(warnings.front().contains(bridge.path()));
  CHECK(warnings.front().starts_with("USART9: "));
  CHECK(bridge.wants_write());
  auto const                client = open_client(bridge.path());
  std::vector<std::uint8_t> got;
  for(auto chunk = read_ready(client, 200); not chunk.empty(); chunk = read_ready(client, 200))
  {
    got.append_range(chunk);
    CHECK_FALSE(bridge.pump().has_value());
  }
  ::close(client);
  REQUIRE(got.size() >= pty_outgoing_limit);
  CHECK(got.size() < 2 * pty_outgoing_limit);
  auto const taken = static_cast<std::size_t>(std::ranges::mismatch(got, sent).in1 - got.begin());
  CHECK(taken < sent.size() / 2);
  CHECK(
      std::ranges::equal(std::span{got}.subspan(taken), std::span{sent}.last(got.size() - taken)));
  CHECK(std::ranges::equal(std::span{got}.last(4096), std::span{sent}.last(4096)));
  CHECK_FALSE(bridge.wants_write());
  CHECK(received.empty());
}

TEST_CASE("host bytes reach the link after one pump and partial writes keep their order")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};
  auto const                client = open_client(bridge.path());
  std::string_view const    line{"ping\n"};
  REQUIRE(::write(client, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
  pollfd ready{.fd = bridge.master(), .events = POLLIN, .revents = 0};
  REQUIRE(::poll(&ready, 1, 1000) == 1);
  std::ignore = bridge.pump();
  CHECK(std::string(received.begin(), received.end()) == line);

  auto const sent = pattern(48 * 1024);
  pending         = sent;
  std::ignore     = bridge.pump();
  CHECK(bridge.wants_write());
  std::vector<std::uint8_t> got = read_ready(client, 1000);
  CHECK_FALSE(bridge.pump().has_value());
  for(auto chunk = read_ready(client, 200); not chunk.empty(); chunk = read_ready(client, 200))
  {
    got.append_range(chunk);
    std::ignore = bridge.pump();
  }
  ::close(client);
  CHECK(got == sent);
  CHECK_FALSE(bridge.wants_write());
}

TEST_CASE("destruction closes both ends of the pty")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  std::string               path;
  {
    pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received)};
    path = bridge.path();
    REQUIRE(holds_pty(path));
    pending     = pattern(1024);
    std::ignore = bridge.pump();
  }
  CHECK_FALSE(holds_pty(path));
}

TEST_CASE("a pty opens again once the last client closed it")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  auto const first = open_client(bridge.path());
  REQUIRE(::ioctl(first, TIOCEXCL) == 0);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == EBUSY);

  release(bridge.path(), first);
  std::ignore      = bridge.pump();
  auto const again = open_client(bridge.path());
  ::close(again);
}

TEST_CASE("a pty stays exclusive until every client closed it")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  auto const early = open_client(bridge.path());
  auto const late  = open_client(bridge.path());
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  release(bridge.path(), late);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == EBUSY);

  release(bridge.path(), early);
  std::ignore = bridge.pump();
  release(bridge.path(), open_client(bridge.path()));
}

TEST_CASE("a non-exclusive client of a link that wants no sessions costs no scan of /proc")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  for(int round = 0; round < 8; ++round)
  {
    release(bridge.path(), open_client(bridge.path()));
    std::ignore = bridge.pump();
  }
  CHECK(bridge.scans() == 0);

  auto const exclusive = open_client(bridge.path());
  REQUIRE(::ioctl(exclusive, TIOCEXCL) == 0);
  release(bridge.path(), exclusive);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(open_error(bridge.path()) == 0);
}

TEST_CASE("a forked child holding the pty keeps it exclusive until it exits")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  std::array<int, 2> gate{};
  REQUIRE(::pipe(gate.data()) == 0);
  auto const client = open_client(bridge.path());
  REQUIRE(::ioctl(client, TIOCEXCL) == 0);
  auto const child = ::fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    ::close(gate[1]);
    char byte   = 0;
    std::ignore = ::read(gate[0], &byte, 1);
    ::_exit(0);
  }
  ::close(gate[0]);
  ::close(client);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == EBUSY);

  ::close(gate[1]);
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == 0);
}

TEST_CASE("the first open of a pty reaches the link and the last close follows")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  std::ignore = bridge.pump();
  CHECK(events.empty());

  auto const first  = open_client(bridge.path());
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  release(bridge.path(), first);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  release(bridge.path(), second);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close"});

  std::ignore = bridge.pump();
  CHECK(events.size() == 2);
}

TEST_CASE("every open and close of a loop reaches the link")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  for(int round = 0; round < 20; ++round)
  {
    release(bridge.path(), open_client(bridge.path()));
    std::ignore = bridge.pump();
  }
  REQUIRE(events.size() == 40);
  for(std::size_t at = 0; at < events.size(); ++at)
  {
    CHECK(events[at] == (at % 2 == 0 ? "open" : "close"));
  }
}

TEST_CASE("an open, a write and a close in one pump reach the link in that order")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  auto const client = open_client(bridge.path());
  REQUIRE(::write(client, "hi", 2) == 2);
  release(bridge.path(), client);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "send hi", "close"});
}

TEST_CASE("a forked child holding the pty keeps the link open until it exits")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  std::array<int, 2> gate{};
  REQUIRE(::pipe(gate.data()) == 0);
  auto const client = open_client(bridge.path());
  auto const child  = ::fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    ::close(gate[1]);
    char byte   = 0;
    std::ignore = ::read(gate[0], &byte, 1);
    ::_exit(0);
  }
  ::close(gate[0]);
  ::close(client);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  ::close(gate[1]);
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close"});
}

TEST_CASE("a close walks /proc once and the next closes re-check the holders found")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  auto const first  = open_client(bridge.path());
  auto const second = open_client(bridge.path());
  auto const third  = open_client(bridge.path());
  std::ignore       = bridge.pump();
  CHECK(bridge.scans() == 0);

  release(bridge.path(), first);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);

  release(bridge.path(), second);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(events == std::vector<std::string>{"open"});

  release(bridge.path(), third);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 2);
  CHECK(events == std::vector<std::string>{"open", "close"});
}

TEST_CASE("an open after a walk leaves the holders found to be re-checked")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  auto const first  = open_client(bridge.path());
  std::ignore       = bridge.pump();
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  release(bridge.path(), first);
  std::ignore = bridge.pump();
  REQUIRE(bridge.scans() == 1);

  auto const third = open_client(bridge.path());
  std::ignore      = bridge.pump();
  release(bridge.path(), third);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(events == std::vector<std::string>{"open"});

  release(bridge.path(), second);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 2);
  CHECK(events == std::vector<std::string>{"open", "close"});
}

namespace
{
auto refused_thread(std::move_only_function<void()>) -> std::jthread
{
  throw std::system_error(EAGAIN, std::generic_category(), "thread");
}

auto cleared_within(std::string const& path, std::chrono::milliseconds limit) -> bool
{
  auto const until = std::chrono::steady_clock::now() + limit;
  while(open_error(path) != 0)
  {
    if(std::chrono::steady_clock::now() > until)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return true;
}

auto exits_cleanly(pid_t child) -> bool
{
  int status = 0;
  return ::waitpid(child, &status, 0) == child and WIFEXITED(status) and WEXITSTATUS(status) == 0;
}

auto open_within(std::string const& path, std::chrono::milliseconds limit) -> bool
{
  auto const deadline = std::chrono::steady_clock::now() + limit;
  for(;;)
  {
    auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd >= 0)
    {
      ::close(fd);
      return true;
    }
    if(errno != EBUSY or std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

auto stuck_rounds(std::string const& path, int rounds) -> int
{
  auto stuck = 0;
  for(int round = 0; round < rounds; ++round)
  {
    auto const holder = ::fork();
    if(holder == 0)
    {
      auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
      ::_exit(fd < 0 or ::ioctl(fd, TIOCEXCL) != 0 ? 2 : 0);
    }
    auto status = 0;
    if(holder < 0 or ::waitpid(holder, &status, 0) != holder or not WIFEXITED(status))
    {
      return 255;
    }
    if(WEXITSTATUS(status) != 0)
    {
      if(not open_within(path, std::chrono::seconds{1}))
      {
        return 255;
      }
      continue;
    }
    stuck += open_within(path, std::chrono::seconds{1}) ? 0 : 1;
  }
  return std::min(stuck, 254);
}

auto queued_events_limit() -> std::size_t
{
  std::ifstream limit{"/proc/sys/fs/inotify/max_queued_events"};
  std::size_t   events = 0;
  limit >> events;
  return events;
}
}

TEST_CASE("the watcher clears a closed tool's exclusive flag with no pump")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};
  REQUIRE(bridge.watching());
  CHECK(bridge.clients_watch() == -1);

  auto const client = open_client(bridge.path());
  REQUIRE(::ioctl(client, TIOCEXCL) == 0);
  CHECK(open_error(bridge.path()) == EBUSY);
  ::close(client);
  CHECK(cleared_within(bridge.path(), std::chrono::seconds{2}));
}

TEST_CASE(
    "a process exiting with the pty exclusive lets the next open succeed within 1 s in every round")
{
  std::vector<std::uint8_t>                pending;
  std::vector<std::uint8_t>                received;
  std::vector<std::string>                 events;
  std::vector<std::unique_ptr<pty_bridge>> bridges;
  bridges.push_back(
      std::make_unique<pty_bridge>("USART1", std::make_unique<fake_link>(pending, received)));
  bridges.push_back(std::make_unique<pty_bridge>("OTG_FS", std::make_unique<session_link>(events)));
  for(auto const& bridge : bridges)
  {
    auto const driver = ::fork();
    REQUIRE(driver >= 0);
    if(driver == 0)
    {
      ::_exit(stuck_rounds(bridge->path(), 200));
    }
    int status = 0;
    REQUIRE(::waitpid(driver, &status, 0) == driver);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }
}

TEST_CASE("a forked child destroying the bridge leaves the parent's watcher running")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  std::optional<pty_bridge> bridge{
      std::in_place, "USART1", std::make_unique<fake_link>(pending, received)};
  REQUIRE(bridge->watching());
  auto const path  = bridge->path();
  auto const child = ::fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    ::alarm(5);
    bridge.reset();
    ::_exit(0);
  }
  CHECK(exits_cleanly(child));

  auto const client = open_client(path);
  REQUIRE(::ioctl(client, TIOCEXCL) == 0);
  ::close(client);
  CHECK(cleared_within(path, std::chrono::seconds{2}));
}

TEST_CASE("a refused watcher thread leaves the clear to the pump with one warning")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};
  CHECK_FALSE(bridge.watching());
  CHECK(bridge.clients_watch() >= 0);
  auto const warned = bridge.pump();
  REQUIRE(warned.has_value());
  CHECK(warned->starts_with("USART1: cannot start the thread watching "));
  CHECK_FALSE(bridge.pump().has_value());

  auto const client = open_client(bridge.path());
  REQUIRE(::ioctl(client, TIOCEXCL) == 0);
  release(bridge.path(), client);
  CHECK(open_error(bridge.path()) == EBUSY);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == 0);
}

TEST_CASE("two opens merged into one event keep the pty exclusive for the tool left")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};

  auto const early = open_client(bridge.path());
  auto const late  = open_client(bridge.path());
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  release(bridge.path(), late);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(open_error(bridge.path()) == EBUSY);

  release(bridge.path(), early);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == 0);
}

TEST_CASE("two closes merged into one event still clear the pty's exclusive flag")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};

  auto const early = open_client(bridge.path());
  std::ignore      = bridge.pump();
  auto const late  = open_client(bridge.path());
  std::ignore      = bridge.pump();
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  release(bridge.path(), early);
  release(bridge.path(), late);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(open_error(bridge.path()) == 0);
}

TEST_CASE("an overflowed event queue walks /proc and clears the flag once no tool holds the pty")
{
  auto const limit = queued_events_limit();
  REQUIRE(limit > 0);
  REQUIRE(limit <= 1'000'000);
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};
  std::ignore = bridge.pump();

  auto const held = open_client(bridge.path());
  for(std::size_t round = 0; round <= limit / 2; ++round)
  {
    ::close(open_client(bridge.path()));
  }
  REQUIRE(::ioctl(held, TIOCEXCL) == 0);
  release(bridge.path(), held);
  CHECK(open_error(bridge.path()) == EBUSY);
  pending     = {'x'};
  std::ignore = bridge.flush();

  std::atomic<bool>        walking{true};
  std::atomic<std::size_t> seen{0};
  std::jthread             reader{[&] {
    while(walking.load())
    {
      seen.store(bridge.scans());
    }
  }};
  std::ignore = bridge.pump();
  walking.store(false);
  reader.join();
  CHECK(bridge.scans() == 1);
  CHECK(seen.load() <= 1);
  auto const next = ::open(bridge.path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  REQUIRE(next >= 0);
  std::array<char, 4> chunk{};
  CHECK(::read(next, chunk.data(), chunk.size()) < 0);
  ::close(next);
}

TEST_CASE("dup and O_PATH opens leave a pty exclusive until its tool closes")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  auto const client = open_client(bridge.path());
  REQUIRE(::ioctl(client, TIOCEXCL) == 0);
  auto const copy = ::dup(client);
  REQUIRE(copy >= 0);
  ::close(copy);
  auto const located = ::open(bridge.path().c_str(), O_PATH | O_CLOEXEC);
  REQUIRE(located >= 0);
  ::close(located);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == EBUSY);

  release(bridge.path(), client);
  CHECK(cleared_within(bridge.path(), std::chrono::seconds{2}));
}

TEST_CASE("a child forked with the bridge's own slave does not hold the pty")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};
  std::ignore = bridge.pump();

  std::array<int, 2> gate{};
  REQUIRE(::pipe(gate.data()) == 0);
  auto const child = ::fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    ::close(gate[1]);
    char byte   = 0;
    std::ignore = ::read(gate[0], &byte, 1);
    ::_exit(0);
  }
  ::close(gate[0]);
  auto const early = open_client(bridge.path());
  auto const late  = open_client(bridge.path());
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  release(bridge.path(), late);
  release(bridge.path(), early);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(open_error(bridge.path()) == 0);

  ::close(gate[1]);
  CHECK(exits_cleanly(child));
}

TEST_CASE("a close and a reopen between two pumps make a close and an open while the watcher runs")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};
  REQUIRE(bridge.watching());

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  release(bridge.path(), first);
  auto const second = open_client(bridge.path());
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close", "open"});

  release(bridge.path(), second);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close", "open", "close"});
}

namespace
{
struct gated_start
{
  std::atomic<bool> open{false};

  auto operator()(std::move_only_function<void()> body) -> std::jthread
  {
    return std::jthread{[this, run = std::move(body)]() mutable {
      open.wait(false);
      run();
    }};
  }

  void release()
  {
    open.store(true);
    open.notify_all();
  }
};

struct gate_guard
{
  gated_start& gate;

  gate_guard(gate_guard const&)            = delete;
  gate_guard& operator=(gate_guard const&) = delete;

  explicit gate_guard(gated_start& started) : gate{started}
  {}

  ~gate_guard()
  {
    gate.release();
  }
};

struct changing_link final : popcorn::bind::detail::pty_link
{
  bool changed = false;

  void send(std::span<std::uint8_t const>) override
  {}

  void drain(std::vector<std::uint8_t>&) override
  {}

  [[nodiscard]] auto device_changed() -> bool override
  {
    return std::exchange(changed, false);
  }
};

auto settles_within(std::chrono::milliseconds limit, std::function_ref<bool()> reached) -> bool
{
  auto const until = std::chrono::steady_clock::now() + limit;
  while(not reached())
  {
    if(std::chrono::steady_clock::now() > until)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds{100});
  }
  return true;
}

void overflow_queue(std::string const& path)
{
  auto const limit = queued_events_limit();
  REQUIRE(limit > 0);
  REQUIRE(limit <= 1'000'000);
  for(std::size_t round = 0; round <= limit / 2; ++round)
  {
    ::close(open_client(path));
  }
  for(auto round = 0; round < 8; ++round)
  {
    release(path, open_client(path));
  }
}
}

TEST_CASE("after an overflow hid a holder's open, a close at count 0 keeps the pty exclusive")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  gated_start               gate;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received), gate};
  REQUIRE(bridge.watching());

  overflow_queue(bridge.path());
  auto const hidden = open_client(bridge.path());
  std::ignore       = bridge.pump();
  gate.release();

  auto const counted = open_client(bridge.path());
  REQUIRE(::ioctl(counted, TIOCEXCL) == 0);
  release(bridge.path(), counted);
  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  CHECK(open_error(bridge.path()) == EBUSY);

  release(bridge.path(), hidden);
  CHECK(cleared_within(bridge.path(), std::chrono::seconds{2}));
}

TEST_CASE("a device change between a fast clear and the watcher's walk leaves the flag clear")
{
  auto        owned = std::make_unique<changing_link>();
  auto&       link  = *owned;
  gated_start gate;
  pty_bridge  bridge{"OTG_FS", std::move(owned), gate};

  auto const early = open_client(bridge.path());
  auto const late  = open_client(bridge.path());
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  release(bridge.path(), late);
  gate.release();
  REQUIRE(settles_within(std::chrono::seconds{2}, [&] {
    auto exclusive = 1;
    return ::ioctl(early, TIOCGEXCL, &exclusive) == 0 and exclusive == 0;
  }));

  link.changed = true;
  std::ignore  = bridge.flush();
  std::this_thread::sleep_for(std::chrono::milliseconds{4 * popcorn::bind::detail::pty_quiet_ms});
  CHECK(open_error(bridge.path()) == 0);
  ::close(early);
}

TEST_CASE("a pump returns while a tool loop keeps interrupting its walk")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), refused_thread};

  auto const        path = bridge.path();
  std::atomic<bool> stop{false};
  std::atomic<bool> expired{false};
  std::atomic<bool> started{false};
  std::jthread      churn{[&] {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while(not stop.load())
    {
      auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
      if(fd >= 0)
      {
        ::close(fd);
      }
      started.store(true);
      if(std::chrono::steady_clock::now() > deadline)
      {
        expired.store(true);
        return;
      }
    }
  }};
  while(not started.load())
  {
    std::this_thread::yield();
  }
  std::ignore = bridge.pump();
  CHECK_FALSE(expired.load());
  stop.store(true);
  churn.join();
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close"});
}

namespace
{
using popcorn::bind::detail::cdc_pty_link;
using popcorn::bind::detail::slave_opener;
namespace usb = popcorn::periph::usb;

constexpr int tool_flags = O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC;

struct cdc_rig
{
  std::shared_ptr<usb::cdc_hub>      hub   = std::make_shared<usb::cdc_hub>();
  int                                posts = 0;
  std::vector<std::pair<bool, bool>> lines;
  std::vector<std::size_t>           bytes_at_post;

  explicit cdc_rig(bool bound)
  {
    if(bound)
    {
      bind();
    }
  }

  void bind()
  {
    auto const held        = hub->lock();
    auto&      channel     = hub->channel(held, 0);
    channel.comm_interface = 0;
    channel.ready          = true;
    channel.line_state     = 3;
    ++channel.attachments;
  }

  void unbind()
  {
    auto const held    = hub->lock();
    auto&      channel = hub->channel(held, 0);
    ++channel.attachments;
    ++channel.unbinds;
    channel.comm_interface.reset();
    channel.ready = false;
  }

  void line_state(std::uint8_t lines)
  {
    auto const held                  = hub->lock();
    hub->channel(held, 0).line_state = lines;
  }

  void restore()
  {
    auto const held    = hub->lock();
    auto&      channel = hub->channel(held, 0);
    ++channel.attachments;
    ++channel.unbinds;
    ++channel.attachments;
  }

  void firmware_sends(std::string_view text)
  {
    auto const                      held = hub->lock();
    std::vector<std::uint8_t> const bytes(text.begin(), text.end());
    hub->channel(held, 0).received(bytes, popcorn::vtime{1});
  }

  auto host_bytes() -> std::size_t
  {
    auto const held = hub->lock();
    return hub->channel(held, 0).out.size();
  }

  auto link()
  {
    std::uint64_t attachments = 0;
    std::uint64_t unbinds     = 0;
    {
      auto const held = hub->lock();
      attachments     = hub->channel(held, 0).attachments;
      unbinds         = hub->channel(held, 0).unbinds;
    }
    return std::make_unique<cdc_pty_link>(hub, 0, attachments, unbinds, [this](bool dtr, bool rts) {
      ++posts;
      lines.emplace_back(dtr, rts);
      bytes_at_post.push_back(host_bytes());
      return true;
    });
  }
};

auto read_errno(int fd) -> int
{
  std::array<char, 16> chunk{};
  return ::read(fd, chunk.data(), chunk.size()) < 0 ? errno : 0;
}

auto write_errno(int fd) -> int
{
  return ::write(fd, "x", 1) < 0 ? errno : 0;
}

auto tool_open(std::string const& path) -> int
{
  return ::open(path.c_str(), tool_flags);
}
}

TEST_CASE("a CDC pty opened before a driver binds refuses opens with EIO until one binds")
{
  cdc_rig    rig{false};
  pty_bridge bridge{"OTG_FS", rig.link()};
  CHECK(bridge.hung_up());
  CHECK(open_error(bridge.path()) == EIO);
  CHECK_FALSE(bridge.pump().has_value());
  CHECK(open_error(bridge.path()) == EIO);

  rig.bind();
  std::ignore = bridge.pump();
  CHECK_FALSE(bridge.hung_up());
  CHECK(open_error(bridge.path()) == 0);
}

TEST_CASE("a detach hangs up a held descriptor and a new open after the re-attach revives it")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  CHECK_FALSE(bridge.hung_up());
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);
  CHECK(read_errno(held) == EAGAIN);
  CHECK(write_errno(held) == 0);

  rig.unbind();
  std::ignore = bridge.pump();
  CHECK(bridge.hung_up());
  CHECK(read_errno(held) == EIO);
  CHECK(write_errno(held) == EIO);
  CHECK(open_error(bridge.path()) == EIO);

  rig.bind();
  std::ignore = bridge.pump();
  CHECK_FALSE(bridge.hung_up());
  CHECK(read_errno(held) == EIO);
  auto const fresh = tool_open(bridge.path());
  REQUIRE(fresh >= 0);
  CHECK(read_errno(fresh) == EAGAIN);
  CHECK(write_errno(fresh) == 0);
  CHECK(read_errno(held) == EAGAIN);
  CHECK(write_errno(held) == 0);
  ::close(fresh);
  ::close(held);
}

TEST_CASE("a bind and an unbind between two pumps hang up once and the next pump releases")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);

  rig.restore();
  std::ignore = bridge.pump();
  CHECK(bridge.hung_up());
  CHECK(read_errno(held) == EIO);
  std::ignore = bridge.pump();
  CHECK_FALSE(bridge.hung_up());
  CHECK(open_error(bridge.path()) == 0);
  ::close(held);
}

TEST_CASE("a restore into a bound machine leaves a held descriptor working")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);

  bridge.discard();
  rig.restore();
  std::ignore = bridge.pump();
  CHECK_FALSE(bridge.hung_up());
  CHECK(read_errno(held) == EAGAIN);
  CHECK(write_errno(held) == 0);

  rig.unbind();
  std::ignore = bridge.pump();
  CHECK(bridge.hung_up());
  CHECK(read_errno(held) == EIO);
  ::close(held);
}

TEST_CASE("a restore into an unbound machine hangs the pty up")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);

  bridge.discard();
  rig.unbind();
  std::ignore = bridge.pump();
  CHECK(bridge.hung_up());
  CHECK(read_errno(held) == EIO);
  ::close(held);
}

TEST_CASE("a tool that sets TIOCEXCL between the clear and the self-open still ends in EIO")
{
  cdc_rig      rig{true};
  auto         calls = 0;
  auto const   held  = std::make_shared<int>(-1);
  slave_opener opener{[&calls, held](char const* path) {
    if(++calls == 1)
    {
      REQUIRE(::ioctl(*held, TIOCEXCL) == 0);
    }
    return ::open(path, tool_flags);
  }};
  pty_bridge   bridge{"OTG_FS", rig.link(), popcorn::bind::detail::start_thread, std::move(opener)};
  *held = tool_open(bridge.path());
  REQUIRE(*held >= 0);

  rig.unbind();
  CHECK_FALSE(bridge.pump().has_value());
  CHECK(calls == 2);
  CHECK(bridge.hung_up());
  CHECK(read_errno(*held) == EIO);
  CHECK(open_error(bridge.path()) == EIO);
  ::close(*held);
}

TEST_CASE("a descriptor holding TIOCEXCL is hung up with no warning")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);
  REQUIRE(::ioctl(held, TIOCEXCL) == 0);

  rig.unbind();
  CHECK_FALSE(bridge.pump().has_value());
  CHECK(read_errno(held) == EIO);
  CHECK(open_error(bridge.path()) == EIO);
  ::close(held);
}

TEST_CASE("a self-open that succeeds is closed, warned once and makes no phantom tool")
{
  cdc_rig      rig{true};
  pty_bridge*  self = nullptr;
  slave_opener opener{[&self](char const* path) {
    int const unlocked = 0;
    REQUIRE(::ioctl(self->master(), TIOCSPTLCK, &unlocked) == 0);
    return ::open(path, tool_flags);
  }};
  pty_bridge   bridge{"OTG_FS", rig.link(), popcorn::bind::detail::start_thread, std::move(opener)};
  self = &bridge;

  rig.unbind();
  auto const warned = bridge.pump();
  REQUIRE(warned.has_value());
  CHECK(warned->contains("did not make the kernel refuse an open"));
  rig.bind();
  std::ignore = bridge.pump();
  rig.unbind();
  CHECK_FALSE(bridge.pump().has_value());
  rig.bind();
  std::ignore = bridge.pump();
  std::this_thread::sleep_for(std::chrono::milliseconds{4 * popcorn::bind::detail::pty_quiet_ms});
  std::ignore = bridge.pump();
  CHECK(rig.posts == 0);
}

TEST_CASE("bytes written before a hang-up never reach a tool opened after the re-attach")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);
  rig.firmware_sends("stale");
  std::ignore = bridge.pump();
  REQUIRE(write_errno(held) == 0);
  rig.firmware_sends("queued");

  rig.unbind();
  std::ignore = bridge.flush();
  rig.firmware_sends("dropped");
  std::ignore = bridge.pump();
  rig.bind();
  std::ignore = bridge.pump();
  CHECK(rig.host_bytes() == 0);

  auto const fresh = tool_open(bridge.path());
  REQUIRE(fresh >= 0);
  CHECK(read_errno(fresh) == EAGAIN);
  rig.firmware_sends("live");
  std::ignore = bridge.pump();
  CHECK(read_ready(fresh, 1000) == std::vector<std::uint8_t>{'l', 'i', 'v', 'e'});
  ::close(fresh);
  ::close(held);
}

TEST_CASE("bytes a tool wrote before an unbind never reach the device bound next")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);
  REQUIRE(::write(held, "ab", 2) == 2);

  rig.unbind();
  std::ignore = bridge.pump();
  rig.bind();
  std::ignore = bridge.pump();
  CHECK(rig.host_bytes() == 0);

  auto const fresh = tool_open(bridge.path());
  REQUIRE(fresh >= 0);
  REQUIRE(::write(fresh, "cd", 2) == 2);
  std::ignore = bridge.pump();
  CHECK(rig.host_bytes() == 2);
  ::close(fresh);
  ::close(held);
}

TEST_CASE("bytes a tool wrote between the bridge's read and its refused open are flushed")
{
  cdc_rig      rig{true};
  auto const   held = std::make_shared<int>(-1);
  slave_opener opener{[held](char const* path) {
    REQUIRE(::write(*held, "late", 4) == 4);
    return ::open(path, tool_flags);
  }};
  pty_bridge   bridge{"OTG_FS", rig.link(), popcorn::bind::detail::start_thread, std::move(opener)};
  *held = tool_open(bridge.path());
  REQUIRE(*held >= 0);

  rig.unbind();
  CHECK_FALSE(bridge.pump().has_value());
  rig.bind();
  std::ignore = bridge.pump();
  CHECK(rig.host_bytes() == 0);
  ::close(*held);
}

TEST_CASE("a UART pty is never hung up")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};
  CHECK_FALSE(bridge.hung_up());
  auto const held = tool_open(bridge.path());
  REQUIRE(held >= 0);
  std::ignore = bridge.pump();
  CHECK_FALSE(bridge.hung_up());
  CHECK(read_errno(held) == EAGAIN);
  CHECK(open_error(bridge.path()) == 0);
  ::close(held);
}

namespace
{
auto open_after_close(std::string const& path) -> int
{
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  for(;;)
  {
    auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if(fd >= 0 or errno != EBUSY or std::chrono::steady_clock::now() > deadline)
    {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

template <typename Send>
void check_unread_bytes_die_at_the_last_close(pty_bridge& bridge, Send send)
{
  for(int round = 0; round < 2; ++round)
  {
    auto const tool = open_client(bridge.path());
    REQUIRE(::ioctl(tool, TIOCEXCL) == 0);
    send("stale");
    std::ignore = bridge.pump();
    REQUIRE(settles_within(std::chrono::seconds{2}, [&] {
      pollfd waiting{.fd = tool, .events = POLLIN, .revents = 0};
      return ::poll(&waiting, 1, 0) > 0;
    }));
    release(bridge.path(), tool);

    auto const next = open_after_close(bridge.path());
    REQUIRE(next >= 0);
    std::array<char, 16> chunk{};
    CHECK(::read(next, chunk.data(), chunk.size()) < 0);
    CHECK(errno == EAGAIN);
    release(bridge.path(), next);
    std::this_thread::sleep_for(std::chrono::milliseconds{4 * popcorn::bind::detail::pty_quiet_ms});
  }
  auto const last = open_after_close(bridge.path());
  REQUIRE(last >= 0);
  std::this_thread::sleep_for(std::chrono::milliseconds{4 * popcorn::bind::detail::pty_quiet_ms});
  send("fresh");
  std::ignore = bridge.pump();
  CHECK(read_ready(last, 1000) == std::vector<std::uint8_t>{'f', 'r', 'e', 's', 'h'});
  ::close(last);
}
}

TEST_CASE("bytes no tool read are dropped at the last close of a UART pty")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};
  check_unread_bytes_die_at_the_last_close(
      bridge, [&](std::string_view text) { pending.assign(text.begin(), text.end()); });
}

TEST_CASE("bytes no tool read are dropped at the last close of a CDC pty")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};
  check_unread_bytes_die_at_the_last_close(
      bridge, [&](std::string_view text) { rig.firmware_sends(text); });
}

TEST_CASE("bytes no tool read are dropped at the last close with no watcher thread")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};
  REQUIRE_FALSE(bridge.watching());
  auto const tool = open_client(bridge.path());
  pending         = {'o', 'l', 'd'};
  std::ignore     = bridge.pump();
  release(bridge.path(), tool);
  std::ignore     = bridge.pump();
  auto const next = tool_open(bridge.path());
  REQUIRE(next >= 0);
  std::array<char, 16> chunk{};
  CHECK(::read(next, chunk.data(), chunk.size()) < 0);
  ::close(next);
}

namespace
{
auto close_together(std::string const& path, std::initializer_list<int> fds) -> int
{
  auto const watch = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  REQUIRE(watch >= 0);
  REQUIRE(::inotify_add_watch(watch, path.c_str(), IN_CLOSE) >= 0);
  for(auto const fd : fds)
  {
    ::close(fd);
  }
  pollfd closed{.fd = watch, .events = POLLIN, .revents = 0};
  REQUIRE(::poll(&closed, 1, 2000) == 1);
  auto                                          events = 0;
  alignas(inotify_event) std::array<char, 1024> buffer{};
  for(;;)
  {
    auto const got = ::read(watch, buffer.data(), buffer.size());
    if(got <= 0)
    {
      break;
    }
    for(std::ptrdiff_t at = 0; at < got;)
    {
      inotify_event event{};
      std::memcpy(&event, buffer.data() + at, sizeof event);
      at += static_cast<std::ptrdiff_t>(sizeof event + event.len);
      events += (event.mask & IN_CLOSE) != 0 ? 1 : 0;
    }
  }
  ::close(watch);
  return events;
}
}

TEST_CASE("two closes merged before the watcher read either still clear the pty's exclusive flag")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  gated_start               gate;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received), gate};

  auto const early = open_client(bridge.path());
  auto const late  = open_client(bridge.path());
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  REQUIRE(close_together(bridge.path(), {early, late}) == 1);
  gate.release();
  CHECK(cleared_within(bridge.path(), std::chrono::seconds{2}));
}

TEST_CASE("after an undercount flushed the bytes of one tool the last close flushes again")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};

  auto const early = tool_open(bridge.path());
  auto const late  = tool_open(bridge.path());
  REQUIRE(early >= 0);
  REQUIRE(late >= 0);
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  std::ignore = bridge.pump();
  release(bridge.path(), late);
  std::ignore = bridge.pump();
  pending     = {'m', 'i', 'd'};
  std::ignore = bridge.pump();
  release(bridge.path(), early);
  std::ignore = bridge.pump();

  auto const next = tool_open(bridge.path());
  REQUIRE(next >= 0);
  std::array<char, 8> chunk{};
  CHECK(::read(next, chunk.data(), chunk.size()) < 0);
  ::close(next);
}

TEST_CASE("a close that leaves a tool holding the pty keeps its unread bytes and its flag")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge bridge{"USART1", std::make_unique<fake_link>(pending, received), refused_thread};

  auto const early = tool_open(bridge.path());
  REQUIRE(early >= 0);
  std::ignore     = bridge.pump();
  auto const late = tool_open(bridge.path());
  REQUIRE(late >= 0);
  REQUIRE(::ioctl(late, TIOCEXCL) == 0);
  std::ignore = bridge.pump();
  pending     = {'k', 'e', 'p', 't'};
  std::ignore = bridge.pump();
  release(bridge.path(), early);
  std::ignore = bridge.pump();

  CHECK(read_ready(late, 1000) == std::vector<std::uint8_t>{'k', 'e', 'p', 't'});
  CHECK(open_error(bridge.path()) == EBUSY);
  ::close(late);
}

TEST_CASE("firmware output written while no tool holds the pty after a close is dropped")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  release(bridge.path(), tool_open(bridge.path()));
  pending     = {'l', 'a', 't', 'e'};
  std::ignore = bridge.pump();

  auto const          next = tool_open(bridge.path());
  std::array<char, 8> chunk{};
  CHECK(::read(next, chunk.data(), chunk.size()) < 0);
  CHECK(errno == EAGAIN);
  ::close(next);
}

TEST_CASE("firmware output written after an open that follows a close reaches the new holder")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  release(bridge.path(), tool_open(bridge.path()));
  auto const next = tool_open(bridge.path());
  pending         = {'n', 'e', 'w'};
  std::ignore     = bridge.pump();
  CHECK(read_ready(next, 1000) == std::vector<std::uint8_t>{'n', 'e', 'w'});
  ::close(next);
}

TEST_CASE("firmware output written before the first open stays buffered for it")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  pending          = {'e', 'a', 'r', 'l', 'y'};
  std::ignore      = bridge.pump();
  auto const first = tool_open(bridge.path());
  CHECK(read_ready(first, 1000) == std::vector<std::uint8_t>{'e', 'a', 'r', 'l', 'y'});
  ::close(first);
}

TEST_CASE("firmware output written while another tool still holds the pty is kept")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  release(bridge.path(), tool_open(bridge.path()));
  auto const holder = tool_open(bridge.path());
  auto const other  = tool_open(bridge.path());
  release(bridge.path(), other);
  pending     = {'k', 'e', 'p', 't'};
  std::ignore = bridge.pump();
  CHECK(read_ready(holder, 1000) == std::vector<std::uint8_t>{'k', 'e', 'p', 't'});
  ::close(holder);
}

TEST_CASE("an open the flush reads before the watcher wakes the host loop")
{
  cdc_rig     rig{true};
  gated_start gate;
  pty_bridge  bridge{"OTG_FS", rig.link(), gate};
  REQUIRE(bridge.watching());

  release(bridge.path(), tool_open(bridge.path()));
  std::ignore     = bridge.pump();
  auto const next = tool_open(bridge.path());
  REQUIRE(next >= 0);
  pollfd wake{.fd = bridge.clients_watch(), .events = POLLIN, .revents = 0};
  REQUIRE(::poll(&wake, 1, 0) == 0);

  rig.firmware_sends("open");
  std::ignore = bridge.flush();
  CHECK(::poll(&wake, 1, 0) == 1);
  CHECK(read_ready(next, 1000) == std::vector<std::uint8_t>{'o', 'p', 'e', 'n'});
  gate.release();
  ::close(next);
}

TEST_CASE("firmware output is kept while the event queue overflowed")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  gated_start               gate;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received), gate};
  REQUIRE(bridge.watching());

  release(bridge.path(), tool_open(bridge.path()));
  std::ignore = bridge.take_sessions();
  overflow_queue(bridge.path());
  pending     = {'k', 'e', 'p', 't'};
  std::ignore = bridge.flush();

  auto const next = tool_open(bridge.path());
  REQUIRE(next >= 0);
  CHECK(read_ready(next, 1000) == std::vector<std::uint8_t>{'k', 'e', 'p', 't'});
  gate.release();
  ::close(next);
}

TEST_CASE("firmware output sent between a re-attach and the next open is dropped after a close")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};

  release(bridge.path(), tool_open(bridge.path()));
  std::ignore = bridge.take_sessions();
  rig.unbind();
  std::ignore = bridge.pump();
  rig.bind();
  std::ignore = bridge.pump();
  rig.firmware_sends("gap");
  std::ignore = bridge.pump();

  auto const next = tool_open(bridge.path());
  REQUIRE(next >= 0);
  CHECK(read_errno(next) == EAGAIN);
  rig.firmware_sends("live");
  std::ignore = bridge.pump();
  CHECK(read_ready(next, 1000) == std::vector<std::uint8_t>{'l', 'i', 'v', 'e'});
  ::close(next);
}

TEST_CASE("a last close and the next open read in one pump reach the link as a close and an open")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  release(bridge.path(), first);
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close", "open"});

  std::ignore = bridge.pump();
  CHECK(events.size() == 3);

  release(bridge.path(), second);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close", "open", "close"});
}

TEST_CASE("a close that leaves a holder and a later open make no close")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  auto const other = open_client(bridge.path());
  std::ignore      = bridge.pump();
  release(bridge.path(), first);
  auto const third = open_client(bridge.path());
  std::ignore      = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});
  release(bridge.path(), other);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});
  release(bridge.path(), third);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close"});
}

TEST_CASE("bytes the next holder wrote in the pump of the cycle follow the open")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  release(bridge.path(), first);
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  auto const second = open_client(bridge.path());
  REQUIRE(::write(second, "hi", 2) == 2);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close", "open", "send hi"});
  ::close(second);
}

TEST_CASE("a CDC pty sends DTR low then high when a last close and an open meet in one pump")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};

  auto const first = tool_open(bridge.path());
  REQUIRE(first >= 0);
  std::ignore = bridge.pump();
  CHECK(rig.lines.empty());

  release(bridge.path(), first);
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  auto const second = tool_open(bridge.path());
  REQUIRE(second >= 0);
  REQUIRE(::write(second, "ab", 2) == 2);
  std::ignore = bridge.pump();
  CHECK(
      rig.lines
      == std::vector<std::pair<bool, bool>>{
          {false, false},
          {true,  true }
  });
  CHECK(rig.bytes_at_post == std::vector<std::size_t>{0, 0});
  CHECK(rig.host_bytes() == 2);
  ::close(second);
}

TEST_CASE("the bytes the closing tool wrote reach the link before the close and the open")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  REQUIRE(::write(first, "aa", 2) == 2);
  release(bridge.path(), first);
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  auto const second = open_client(bridge.path());
  REQUIRE(::write(second, "bb", 2) == 2);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "send aa", "close", "open", "send bb"});
  ::close(second);
}

TEST_CASE("a CDC pty sends the closing tool's bytes, then DTR low and high, then the next tool's")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};

  auto const first = tool_open(bridge.path());
  REQUIRE(first >= 0);
  std::ignore = bridge.pump();
  REQUIRE(::write(first, "aa", 2) == 2);
  release(bridge.path(), first);
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  auto const second = tool_open(bridge.path());
  REQUIRE(second >= 0);
  REQUIRE(::write(second, "bb", 2) == 2);
  std::ignore = bridge.pump();
  CHECK(
      rig.lines
      == std::vector<std::pair<bool, bool>>{
          {false, false},
          {true,  true }
  });
  CHECK(rig.bytes_at_post == std::vector<std::size_t>{2, 2});
  CHECK(rig.host_bytes() == 4);
  ::close(second);
}

TEST_CASE("a closing tool's bytes precede the DTR cycle when the next tool opens with no write")
{
  cdc_rig    rig{true};
  pty_bridge bridge{"OTG_FS", rig.link()};

  auto const first = tool_open(bridge.path());
  REQUIRE(first >= 0);
  std::ignore = bridge.pump();
  REQUIRE(::write(first, "x", 1) == 1);
  release(bridge.path(), first);
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  auto const second = tool_open(bridge.path());
  REQUIRE(second >= 0);
  std::ignore = bridge.pump();
  CHECK(rig.bytes_at_post == std::vector<std::size_t>{1, 1});
  CHECK(rig.host_bytes() == 1);
  ::close(second);
}

TEST_CASE("bytes a closing tool wrote reach the link first when no open follows")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  REQUIRE(::write(first, "z", 1) == 1);
  release(bridge.path(), first);
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "send z", "close"});
}

TEST_CASE("a close that left a holder the walk found keeps the cycle for the next open")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first  = open_client(bridge.path());
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});
  release(bridge.path(), first);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  auto const third = open_client(bridge.path());
  std::ignore      = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close", "open"});
  ::close(second);
  ::close(third);
}

TEST_CASE("an open after a close with no tool held at the start of the pump makes an open only")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  release(bridge.path(), open_client(bridge.path()));
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});
  ::close(second);
}

TEST_CASE("a close that leaves a holder after a cycle makes no further close")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  release(bridge.path(), first);
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  REQUIRE(events == std::vector<std::string>{"open", "close", "open"});

  auto const third = open_client(bridge.path());
  std::ignore      = bridge.pump();
  release(bridge.path(), second);
  auto const fourth = open_client(bridge.path());
  std::ignore       = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close", "open"});
  ::close(third);
  ::close(fourth);
}

TEST_CASE("a cycle read in the same batch as an event queue overflow makes no edge")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  release(bridge.path(), first);
  auto const second = open_client(bridge.path());
  overflow_queue(bridge.path());
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});
  ::close(second);
}

TEST_CASE("a close and an open after an overflow was verified make no cycle")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  overflow_queue(bridge.path());
  std::ignore = bridge.pump();
  REQUIRE(events == std::vector<std::string>{"open"});

  release(bridge.path(), first);
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});
  ::close(second);
}

TEST_CASE(
    "an open while a forked child holds the pty after the tool that opened it closed makes no "
    "cycle")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  std::array<int, 2> hold{};
  REQUIRE(::pipe(hold.data()) == 0);
  auto const child = ::fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    ::close(hold[1]);
    char byte   = 0;
    std::ignore = ::read(hold[0], &byte, 1);
    ::_exit(0);
  }
  ::close(hold[0]);
  ::close(first);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  auto const third = open_client(bridge.path());
  std::ignore      = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  ::close(hold[1]);
  CHECK(exits_cleanly(child));
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});
  release(bridge.path(), third);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close"});
}

TEST_CASE(
    "a child that opens the pty itself is a holder and an open after the first tool closes makes "
    "no cycle")
{
  std::vector<std::string> events;
  gated_start              gate;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events), gate};
  gate_guard               guard{gate};

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  std::array<int, 2> hold{};
  std::array<int, 2> ready{};
  REQUIRE(::pipe(hold.data()) == 0);
  REQUIRE(::pipe(ready.data()) == 0);
  auto const child = ::fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    ::close(first);
    ::close(hold[1]);
    ::close(ready[0]);
    auto const own  = ::open(bridge.path().c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    char       byte = own >= 0 ? 'y' : 'n';
    std::ignore     = ::write(ready[1], &byte, 1);
    std::ignore     = ::read(hold[0], &byte, 1);
    ::_exit(0);
  }
  ::close(hold[0]);
  ::close(ready[1]);
  char opened = 0;
  REQUIRE(::read(ready[0], &opened, 1) == 1);
  REQUIRE(opened == 'y');
  std::ignore = bridge.pump();
  release(bridge.path(), first);
  auto const third = open_client(bridge.path());
  std::ignore      = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  ::close(third);
  ::close(hold[1]);
  ::close(ready[0]);
  CHECK(exits_cleanly(child));
}
