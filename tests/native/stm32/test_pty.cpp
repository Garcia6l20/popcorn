#include "pty.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
  CHECK(::open(bridge.path().c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC) == -1);
  CHECK(errno == EBUSY);

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
  CHECK(::open(bridge.path().c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC) == -1);
  CHECK(errno == EBUSY);

  ::close(early);
  std::ignore = bridge.pump();
  ::close(open_client(bridge.path()));
}
