#include "pty.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <poll.h>
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

auto open_error(std::string const& path) -> int
{
  auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
  if(fd >= 0)
  {
    ::close(fd);
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
  ::close(client);
  CHECK_FALSE(bridge.pump().has_value());
  auto const again = open_client(bridge.path());
  for(auto chunk = read_ready(again, 200); not chunk.empty(); chunk = read_ready(again, 200))
  {
    got.append_range(chunk);
    std::ignore = bridge.pump();
  }
  ::close(again);
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

  ::close(first);
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
  ::close(late);
  std::ignore = bridge.pump();
  CHECK(open_error(bridge.path()) == EBUSY);

  ::close(early);
  std::ignore = bridge.pump();
  ::close(open_client(bridge.path()));
}

TEST_CASE("a non-exclusive client of a link that wants no sessions costs no scan of /proc")
{
  std::vector<std::uint8_t> pending;
  std::vector<std::uint8_t> received;
  pty_bridge                bridge{"USART1", std::make_unique<fake_link>(pending, received)};

  for(int round = 0; round < 8; ++round)
  {
    ::close(open_client(bridge.path()));
    std::ignore = bridge.pump();
  }
  CHECK(bridge.scans() == 0);

  auto const exclusive = open_client(bridge.path());
  REQUIRE(::ioctl(exclusive, TIOCEXCL) == 0);
  ::close(exclusive);
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

  ::close(first);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  ::close(second);
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
    ::close(open_client(bridge.path()));
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
  ::close(client);
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

  ::close(first);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);

  ::close(second);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(events == std::vector<std::string>{"open"});

  ::close(third);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 2);
  CHECK(events == std::vector<std::string>{"open", "close"});
}

TEST_CASE("an open after a walk leaves the holders found to be re-checked")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};

  auto const first  = open_client(bridge.path());
  auto const second = open_client(bridge.path());
  std::ignore       = bridge.pump();
  ::close(first);
  std::ignore = bridge.pump();
  REQUIRE(bridge.scans() == 1);

  auto const third = open_client(bridge.path());
  std::ignore      = bridge.pump();
  ::close(third);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(events == std::vector<std::string>{"open"});

  ::close(second);
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

auto busy_rounds(std::string const& path, int rounds) -> int
{
  int busy = 0;
  for(int round = 0; round < rounds; ++round)
  {
    auto const holder = ::fork();
    if(holder == 0)
    {
      auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
      ::_exit(fd < 0 ? (errno == EBUSY ? 1 : 2) : ::ioctl(fd, TIOCEXCL) != 0 ? 2 : 0);
    }
    auto status = 0;
    if(holder < 0 or ::waitpid(holder, &status, 0) != holder or not WIFEXITED(status))
    {
      return 255;
    }
    if(WEXITSTATUS(status) != 0)
    {
      if(WEXITSTATUS(status) != 1)
      {
        return 255;
      }
      ++busy;
      continue;
    }
    auto const next = ::fork();
    if(next == 0)
    {
      ::execl("/bin/sh", "sh", "-c", "exec 3<>\"$0\"", path.c_str(), nullptr);
      ::_exit(127);
    }
    if(next < 0)
    {
      return 255;
    }
    busy += exits_cleanly(next) ? 0 : 1;
  }
  return std::min(busy, 254);
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
    "a process exiting with the pty exclusive lets the next process open it in 196 of 200 rounds")
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
      ::_exit(busy_rounds(bridge->path(), 200));
    }
    int status = 0;
    REQUIRE(::waitpid(driver, &status, 0) == driver);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) <= 4);
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
  ::close(client);
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
  ::close(late);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(open_error(bridge.path()) == EBUSY);

  ::close(early);
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
  ::close(early);
  ::close(late);
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
  ::close(held);
  CHECK(open_error(bridge.path()) == EBUSY);

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
  CHECK(open_error(bridge.path()) == 0);
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

  ::close(client);
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
  ::close(late);
  ::close(early);
  std::ignore = bridge.pump();
  CHECK(bridge.scans() == 1);
  CHECK(open_error(bridge.path()) == 0);

  ::close(gate[1]);
  CHECK(exits_cleanly(child));
}

TEST_CASE("a close and a reopen between two pumps make no change while the watcher runs")
{
  std::vector<std::string> events;
  pty_bridge               bridge{"OTG_FS", std::make_unique<session_link>(events)};
  REQUIRE(bridge.watching());

  auto const first = open_client(bridge.path());
  std::ignore      = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  ::close(first);
  auto const second = open_client(bridge.path());
  std::this_thread::sleep_for(std::chrono::milliseconds{2 * popcorn::bind::detail::pty_quiet_ms});
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open"});

  ::close(second);
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close"});
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
  ::close(counted);
  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  CHECK(open_error(bridge.path()) == EBUSY);

  ::close(hidden);
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
  ::close(late);
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
  std::atomic<bool> churning{true};
  std::atomic<bool> started{false};
  std::jthread      churn{[&] {
    for(int round = 0; round < 20'000; ++round)
    {
      auto const fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
      if(fd >= 0)
      {
        ::close(fd);
      }
      started.store(true);
    }
    churning.store(false);
  }};
  while(not started.load())
  {
    std::this_thread::yield();
  }
  std::ignore = bridge.pump();
  CHECK(churning.load());
  churn.join();
  std::ignore = bridge.pump();
  CHECK(events == std::vector<std::string>{"open", "close"});
}
