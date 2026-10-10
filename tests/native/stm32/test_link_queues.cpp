#include "link_queues.hpp"

#include <popcorn/core/link_guard.hpp>
#include <popcorn/periph/usb_cdc_acm.hpp>
#include <popcorn/periph/usb_hid.hpp>
#include <popcorn/periph/usb_uac2.hpp>

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <tuple>
#include <vector>

namespace
{
namespace detail = popcorn::bind::detail;
namespace usb    = popcorn::periph::usb;
using popcorn::vtime;
using popcorn::core::link_guard;

constexpr popcorn::periph::stm32::audio_format stereo{.data_bits = 16, .active_slots = 3};
}

TEST_CASE("a link guard counts the link locks its thread holds, a held lock refuses try_lock")
{
  detail::queue_endpoint const queue;
  CHECK(popcorn::core::detail::link_locks_held == 0);
  {
    auto const held = queue.lock();
    CHECK(popcorn::core::detail::link_locks_held == 1);
    std::thread{[&] {
      auto const refused = queue.try_lock();
      CHECK_FALSE(refused.owns_lock());
      CHECK(popcorn::core::detail::link_locks_held == 0);
    }}.join();
  }
  CHECK(popcorn::core::detail::link_locks_held == 0);
  auto const taken = queue.try_lock();
  CHECK(taken.owns_lock());
  CHECK(popcorn::core::detail::link_locks_held == 1);
}

TEST_CASE("a UART queue filled on one thread and drained on another loses and reorders nothing")
{
  constexpr std::uint64_t sent = 1'000'000;
  detail::queue_endpoint  queue;
  queue.set_limit(queue.lock(), 1000);
  std::atomic<bool>          done{false};
  std::vector<std::uint64_t> times;
  std::thread                drainer{[&] {
    for(;;)
    {
      auto const finished = done.load(std::memory_order_acquire);
      auto const batch    = [&] {
        auto const held = queue.lock();
        return queue.take(held, 257);
      }();
      for(auto const& frame : batch)
      {
        times.push_back(frame.at_ps);
      }
      if(finished and batch.empty())
      {
        return;
      }
    }
  }};
  for(std::uint64_t at = 0; at < sent; ++at)
  {
    queue.transmitted(static_cast<std::uint16_t>(at & 0x1ff), vtime{at});
  }
  done.store(true, std::memory_order_release);
  drainer.join();
  auto const held = queue.lock();
  CHECK(queue.available(held) == 0);
  CHECK(times.size() + queue.dropped(held) == sent);
  CHECK(std::ranges::is_sorted(times));
  CHECK(std::ranges::adjacent_find(times) == times.end());
  REQUIRE_FALSE(times.empty());
  CHECK(times.back() == sent - 1);
}

TEST_CASE("an audio queue moves frames and written words across threads without loss")
{
  constexpr std::uint64_t      frames = 200'000;
  constexpr std::uint32_t      words  = 100'000;
  detail::audio_queue_endpoint queue;
  queue.set_limit(queue.lock(), 64);
  std::atomic<bool>          done{false};
  std::vector<std::uint64_t> starts;
  std::thread                binding{[&] {
    std::uint32_t next = 1;
    for(;;)
    {
      auto const finished = done.load(std::memory_order_acquire);
      auto const batch    = [&] {
        auto const                 held = queue.lock();
        std::vector<std::uint32_t> chunk;
        while(chunk.size() < 8 and next <= words)
        {
          chunk.push_back(next++);
        }
        queue.write(held, chunk);
        return queue.take(held, 33);
      }();
      for(auto const& frame : batch)
      {
        CHECK(frame.slots.size() == 2);
        CHECK(frame.format == stereo);
        starts.push_back(frame.at_ps);
      }
      if(finished and batch.empty())
      {
        return;
      }
    }
  }};
  std::vector<std::uint32_t> received;
  for(std::uint64_t at = 0; at < frames; ++at)
  {
    std::array<std::uint32_t, 2> const slots{static_cast<std::uint32_t>(at), 0};
    queue.transmitted(slots, stereo, vtime{at}, vtime{at + 1});
    std::array<std::uint32_t, 2> in{};
    queue.receive(in, stereo, vtime{at});
    for(auto const word : in)
    {
      if(word != 0)
      {
        received.push_back(word);
      }
    }
  }
  done.store(true, std::memory_order_release);
  binding.join();
  auto const held = queue.lock();
  CHECK(starts.size() + queue.dropped(held) == frames);
  CHECK(std::ranges::is_sorted(starts));
  CHECK(std::ranges::adjacent_find(starts) == starts.end());
  CHECK(std::ranges::is_sorted(received));
  CHECK(std::ranges::adjacent_find(received) == received.end());
  CHECK(received.size() + queue.queued(held) <= words);
}

TEST_CASE("an audio queue discarded and trimmed to 1 while the engine fills it stays consistent")
{
  detail::audio_queue_endpoint queue;
  std::atomic<bool>            done{false};
  std::thread                  binding{[&] {
    for(std::uint64_t round = 0; not done.load(std::memory_order_acquire); ++round)
    {
      auto const held = queue.lock();
      if(round % 3 == 0)
      {
        queue.discard(held);
      }
      else
      {
        queue.set_limit(held, 1 + round % 2);
        CHECK(queue.available(held) <= 2);
      }
    }
  }};
  for(std::uint64_t at = 0; at < 200'000; ++at)
  {
    std::array<std::uint32_t, 2> const slots{1, 2};
    queue.transmitted(slots, stereo, vtime{at}, vtime{at + 1});
  }
  done.store(true, std::memory_order_release);
  binding.join();
  auto const held  = queue.lock();
  auto const taken = queue.take(held, 10);
  CHECK(taken.size() <= 2);
  for(auto const& frame : taken)
  {
    CHECK(frame.slots == std::vector<std::uint32_t>{1, 2});
  }
}

TEST_CASE("a UART queue drains 0 frames, at most what it holds, and a limit of 1 keeps the newest")
{
  detail::queue_endpoint queue;
  for(std::uint16_t data = 0; data < 5; ++data)
  {
    queue.transmitted(data, vtime{data});
  }
  {
    auto const held = queue.lock();
    CHECK(queue.take(held, 0).empty());
    CHECK(queue.available(held) == 5);
    CHECK(queue.take(held, 2).size() == 2);
    CHECK(queue.take(held, 100).size() == 3);
    CHECK(queue.take(held, 100).empty());
  }
  for(std::uint16_t data = 0; data < 5; ++data)
  {
    queue.transmitted(data, vtime{data});
  }
  queue.set_limit(queue.lock(), 1);
  queue.transmitted(9, vtime{9});
  auto const held = queue.lock();
  CHECK(queue.dropped(held) == 5);
  auto const kept = queue.take(held, 100);
  REQUIRE(kept.size() == 1);
  CHECK(kept[0].data == 9);
}

TEST_CASE("a CDC hub serves one channel while another thread inserts channels into its map")
{
  constexpr std::uint64_t packets = 100'000;
  usb::cdc_hub            hub;
  std::atomic<bool>       done{false};
  std::thread             inserter{[&] {
    for(std::size_t function = 1; not done.load(std::memory_order_acquire); ++function)
    {
      auto const held  = hub.lock();
      auto&      fresh = hub.channel(held, function % 4096 + 1);
      fresh.out.push_back(static_cast<std::uint8_t>(function));
      fresh.ready = not fresh.ready;
    }
  }};
  std::uint64_t           taken = 0;
  for(std::uint64_t at = 0; at < packets; ++at)
  {
    std::array<std::uint8_t, 3> const data{1, 2, 3};
    auto const                        held    = hub.lock();
    auto&                             channel = hub.channel(held, 0);
    channel.received(data, vtime{at});
    if(at % 7 == 0)
    {
      for(auto const& packet : channel.take(10))
      {
        taken += packet.data.size();
      }
    }
  }
  done.store(true, std::memory_order_release);
  inserter.join();
  auto const held    = hub.lock();
  auto&      channel = hub.channel(held, 0);
  CHECK(taken + channel.in_bytes + channel.dropped == 3 * packets);
  CHECK(channel.out.empty());
}

TEST_CASE("a UART queue peek copies low bytes from an offset and leaves the queue as it was")
{
  detail::queue_endpoint queue;
  {
    auto const held = queue.lock();
    CHECK(queue.peek(held, 0, 8).empty());
  }
  queue.transmitted(0x1A5, vtime{1});
  queue.transmitted(0x42, vtime{2});
  queue.transmitted(0x43, vtime{3});
  auto const held = queue.lock();
  CHECK(queue.peek(held, 0, 8) == std::vector<std::uint8_t>{0xA5, 0x42, 0x43});
  CHECK(queue.peek(held, 1, 1) == std::vector<std::uint8_t>{0x42});
  CHECK(queue.peek(held, 0, 0).empty());
  CHECK(queue.peek(held, 3, 8).empty());
  CHECK(queue.peek(held, 100, 8).empty());
  CHECK(queue.available(held) == 3);
  auto const kept = queue.take(held, 3);
  REQUIRE(kept.size() == 3);
  CHECK(kept[0].data == 0x1A5);
}

TEST_CASE("a CDC channel peek spans packets, skips bytes taken, and leaves the channel as it was")
{
  usb::cdc_channel channel;
  CHECK(channel.peek(0, 8).empty());
  constexpr std::array<std::uint8_t, 3> first{1, 2, 3};
  constexpr std::array<std::uint8_t, 2> second{4, 5};
  channel.received(first, vtime{1});
  channel.received(second, vtime{2});
  CHECK(channel.peek(0, 8) == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
  CHECK(channel.peek(2, 2) == std::vector<std::uint8_t>{3, 4});
  CHECK(channel.peek(0, 0).empty());
  CHECK(channel.peek(5, 8).empty());
  std::ignore = channel.take(1);
  CHECK(channel.peek(0, 8) == std::vector<std::uint8_t>{2, 3, 4, 5});
  CHECK(channel.peek(3, 8) == std::vector<std::uint8_t>{5});
  CHECK(channel.in_bytes == 4);
  CHECK(channel.packets() == 2);
}

TEST_CASE("a UART queue taken until a time stops at the first later frame and keeps the rest")
{
  detail::queue_endpoint queue;
  for(std::uint64_t at = 10; at <= 50; at += 10)
  {
    queue.transmitted(static_cast<std::uint16_t>(at), vtime{at});
  }
  auto const held  = queue.lock();
  auto const first = queue.take(held, 8, 30);
  REQUIRE(first.size() == 3);
  CHECK(first.back().at_ps == 30);
  CHECK(queue.take(held, 8, 39).empty());
  auto const next = queue.take(held, 1, 100);
  REQUIRE(next.size() == 1);
  CHECK(next.front().at_ps == 40);
  CHECK(queue.available(held) == 1);
}

TEST_CASE("a CDC channel taken until a time splits only packets received by then")
{
  usb::cdc_channel                      channel;
  constexpr std::array<std::uint8_t, 3> first{1, 2, 3};
  constexpr std::array<std::uint8_t, 2> second{4, 5};
  channel.received(first, vtime{10});
  channel.received(second, vtime{20});
  auto const part = channel.take(2, vtime{15});
  REQUIRE(part.size() == 1);
  CHECK(part.front().data == std::vector<std::uint8_t>{1, 2});
  auto const rest = channel.take(8, vtime{15});
  REQUIRE(rest.size() == 1);
  CHECK(rest.front().data == std::vector<std::uint8_t>{3});
  CHECK(channel.take(8, vtime{19}).empty());
  CHECK(channel.take_packets(8, vtime{19}).empty());
  CHECK(channel.in_bytes == 2);
  auto const packets = channel.take_packets(8, vtime{20});
  REQUIRE(packets.size() == 1);
  CHECK(packets.front().at == vtime{20});
}

TEST_CASE("an audio queue taken until a time takes the frames that ended by then")
{
  detail::audio_queue_endpoint       queue;
  std::array<std::uint32_t, 2> const slots{1, 2};
  queue.transmitted(slots, stereo, vtime{0}, vtime{64});
  queue.transmitted(slots, stereo, vtime{64}, vtime{128});
  auto const held = queue.lock();
  CHECK(queue.take(held, 8, 100).size() == 1);
  CHECK(queue.take(held, 8, 127).empty());
  auto const rest = queue.take(held, 8, 128);
  REQUIRE(rest.size() == 1);
  CHECK(rest.front().at_ps == 64);
}

TEST_CASE("HID reports and microphone frames are taken by the time the host queued them")
{
  constexpr std::array<std::uint8_t, 1> report{7};
  usb::hid_channel                      hid;
  hid.received(report, vtime{30}, vtime{10});
  hid.received(report, vtime{60}, vtime{50});
  auto const reports = hid.take(8, vtime{20});
  REQUIRE(reports.size() == 1);
  CHECK(reports.front().at == vtime{30});
  CHECK(hid.take(8, vtime{49}).empty());
  CHECK(hid.in.size() == 1);
  usb::uac2_stream mic;
  mic.received({.samples = {}, .at = vtime{30}, .ready = vtime{10}});
  mic.received({.samples = {}, .at = vtime{60}, .ready = vtime{50}});
  auto const frames = mic.take(8, vtime{20});
  REQUIRE(frames.size() == 1);
  CHECK(frames.front().at == vtime{30});
  CHECK(mic.take(8, vtime{49}).empty());
  CHECK(mic.in.size() == 1);
}
