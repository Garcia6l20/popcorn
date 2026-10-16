#include "arch_fixture.hpp"
#include "cortex_m.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::bus_error;
using popcorn::initiator;
using popcorn::stop_reason;
using popcorn::arch::fault_policy;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33_with;
using popcorn::testing::sym;

constexpr address target = 0x0800'4000;

struct seen_read
{
  std::string  region;
  address      addr{};
  std::uint8_t size{};
  initiator    by{};

  friend auto operator==(seen_read const&, seen_read const&) -> bool = default;
};

struct seen_store
{
  std::string   region;
  address       addr{};
  std::uint8_t  size{};
  std::uint32_t value{};
  initiator     by{};

  friend auto operator==(seen_store const&, seen_store const&) -> bool = default;
};

class store_model final : public periph::model
{
public:
  explicit store_model(periph::model_context& ctx) : ctx_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, bus_error> override
  {
    return 0u;
  }

  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, bus_error> override
  {
    return {};
  }

  auto on_memory_write(
      popcorn::region_desc const& region,
      address                     addr,
      std::uint8_t                size,
      std::uint32_t               value,
      periph::access_ctx          ctx) -> std::expected<void, bus_error> override
  {
    stores.push_back(
        {.region = region.name, .addr = addr, .size = size, .value = value, .by = ctx.by});
    if(on_store)
    {
      on_store(*this);
    }
    if(refuse)
    {
      return std::unexpected{bus_error::denied};
    }
    if(write_through)
    {
      std::array<std::byte, 4> bytes{};
      for(std::size_t i = 0; i < size; ++i)
      {
        bytes[i] = static_cast<std::byte>(value >> (8 * i));
      }
      REQUIRE(ctx_.services.write_block(addr, std::span{bytes}.first(size), initiator::debugger));
    }
    return {};
  }

  void on_memory_read(
      popcorn::region_desc const& region,
      address                     addr,
      std::uint8_t                size,
      periph::access_ctx          ctx) override
  {
    reads.push_back({.region = region.name, .addr = addr, .size = size, .by = ctx.by});
    if(on_load)
    {
      on_load(*this);
    }
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>& out) const override
  {
    for(auto const& [at, size] : watched)
    {
      for(auto const word : {at, size})
      {
        for(std::size_t i = 0; i < 4; ++i)
        {
          out.push_back(static_cast<std::byte>(word >> (8 * i)));
        }
      }
    }
  }

  void load(std::span<std::byte const> in) override
  {
    if(not rebuild_on_load)
    {
      return;
    }
    std::vector<std::uint32_t> words;
    for(std::size_t at = 0; at + 4 <= in.size(); at += 4)
    {
      std::uint32_t word = 0;
      for(std::size_t i = 0; i < 4; ++i)
      {
        word |= std::to_integer<std::uint32_t>(in[at + i]) << (8 * i);
      }
      words.push_back(word);
    }
    watched.clear();
    for(std::size_t i = 0; i + 1 < words.size(); i += 2)
    {
      watch(words[i], words[i + 1]);
    }
  }

  void watch(address at, std::uint32_t size, bool on = true)
  {
    ctx_.services.watch_reads(at, size, on);
    if(on)
    {
      watched.emplace_back(at, size);
    }
    else
    {
      std::erase(watched, std::pair{at, size});
    }
  }

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return ctx_.services;
  }

  [[nodiscard]] auto context() const -> periph::model_context&
  {
    return ctx_;
  }

  std::vector<seen_store>                        stores{};
  std::vector<seen_read>                         reads{};
  std::function<void(store_model&)>              on_load{};
  std::function<void(store_model&)>              on_store{};
  std::vector<std::pair<address, std::uint32_t>> watched{};
  bool                                           refuse          = false;
  bool                                           write_through   = false;
  bool                                           rebuild_on_load = false;

private:
  periph::model_context& ctx_;
};

auto fn(std::string_view name) -> address
{
  return sym("rom_store", name);
}

struct machine_under
{
  std::shared_ptr<store_model>  model{};
  std::unique_ptr<arch_fixture> f{};
};

auto booted(bool with_model = true) -> machine_under
{
  machine_under         out;
  core::machine_options options;
  options.faults = fault_policy::stop;
  if(with_model)
  {
    options.models.emplace(
        "TESTDEV",
        [&out](periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> {
          out.model = std::make_shared<store_model>(ctx);
          return out.model;
        });
  }
  auto desc = m33_with([](popcorn::part_description& d) { d.regions[0].controller = "TESTDEV"; });
  out.f     = std::make_unique<arch_fixture>(std::move(desc), "rom_store", std::move(options));
  REQUIRE(out.f->run_to_stop().reason == stop_reason::user);
  return out;
}

auto store(
    machine_under&   m,
    std::string_view entry,
    address          at,
    std::uint32_t    low,
    std::uint32_t    high = 0) -> popcorn::stop
{
  m.f->set("r0", 0);
  m.f->set("r1", low);
  m.f->set("r2", at);
  m.f->set("r3", high);
  m.f->set("pc", fn(entry));
  return m.f->run_to_stop();
}

auto flash_word(machine_under const& m, address at) -> std::uint32_t
{
  return m.f->word(at);
}

constexpr address watched_at = target + 0x10;

auto load(machine_under& m, std::string_view entry, address at) -> popcorn::stop
{
  m.f->set("r0", 0);
  m.f->set("r2", at);
  m.f->set("pc", fn(entry));
  return m.f->run_to_stop({.max_instructions = popcorn::icount{64}});
}

auto loads(machine_under& m, address at) -> std::vector<seen_read>
{
  m.model->reads.clear();
  REQUIRE(load(m, "ldr_entry", at).reason == stop_reason::user);
  REQUIRE(m.f->reg("r0") == 1);
  return m.model->reads;
}

auto cpu_read(address at, std::uint8_t size = 4, std::string region = "FLASH") -> seen_read
{
  return {.region = std::move(region), .addr = at, .size = size, .by = initiator::cpu};
}
}

TEST_CASE("a store into a controlled region reaches its model, is dropped, and the code goes on")
{
  auto       m      = booted();
  auto const before = flash_word(m, target);
  auto const stop   = store(m, "str_entry", target, 0x1234'5678);
  CHECK(stop.reason == stop_reason::user);
  CHECK(m.f->reg("r0") == 1);
  CHECK(
      m.model->stores
      == std::vector<seen_store>{
          {"FLASH", target, 4, 0x1234'5678, initiator::cpu}
  });
  CHECK(flash_word(m, target) == before);
}

TEST_CASE("halfword and byte stores keep their size and value")
{
  auto m = booted();
  REQUIRE(store(m, "strh_entry", target + 2, 0xabcd'1234).reason == stop_reason::user);
  REQUIRE(store(m, "strb_entry", target + 1, 0xabcd'1234).reason == stop_reason::user);
  CHECK(
      m.model->stores
      == std::vector<seen_store>{
          {"FLASH", target + 2, 2, 0x1234, initiator::cpu},
          {"FLASH", target + 1, 1, 0x34,   initiator::cpu}
  });
}

TEST_CASE("STRD and two STR reach the model as two words, low address first")
{
  for(auto const entry : {std::string_view{"strd_entry"}, std::string_view{"two_entry"}})
  {
    CAPTURE(entry);
    auto m = booted();
    REQUIRE(store(m, entry, target, 0x1111'1111, 0x2222'2222).reason == stop_reason::user);
    CHECK(m.f->reg("r0") == 1);
    CHECK(
        m.model->stores
        == std::vector<seen_store>{
            {"FLASH", target,     4, 0x1111'1111, initiator::cpu},
            {"FLASH", target + 4, 4, 0x2222'2222, initiator::cpu}
    });
  }
}

TEST_CASE("a VSTR.64 is split into two words, low address first")
{
  auto m = booted();
  REQUIRE(store(m, "vstr_entry", target, 0x1111'1111, 0x2222'2222).reason == stop_reason::user);
  CHECK(m.f->reg("r0") == 1);
  CHECK(
      m.model->stores
      == std::vector<seen_store>{
          {"FLASH", target,     4, 0x1111'1111, initiator::cpu},
          {"FLASH", target + 4, 4, 0x2222'2222, initiator::cpu}
  });
}

TEST_CASE("a bus error from the model is an imprecise bus fault, and skips the second VSTR.64 word")
{
  for(auto const entry : {std::string_view{"str_entry"}, std::string_view{"vstr_entry"}})
  {
    CAPTURE(entry);
    auto m          = booted();
    m.model->refuse = true;
    auto const stop = store(m, entry, target, 0x1111'1111, 0x2222'2222);
    REQUIRE(stop.reason == stop_reason::fault);
    CHECK(stop.fault->kind == popcorn::fault_kind::bus);
    CHECK(m.model->stores.size() == 1);
  }
}

TEST_CASE("the model writes the region through the debugger")
{
  auto m                 = booted();
  m.model->write_through = true;
  REQUIRE(store(m, "str_entry", target, 0x1234'5678).reason == stop_reason::user);
  CHECK(flash_word(m, target) == 0x1234'5678u);
}

TEST_CASE("DMA stores take the same path, block writes from a bus master are refused")
{
  auto  m        = booted();
  auto& services = m.model->services();
  CHECK(services.write(target, 4, 0xa5a5'a5a5, initiator::dma));
  CHECK(
      m.model->stores
      == std::vector<seen_store>{
          {"FLASH", target, 4, 0xa5a5'a5a5, initiator::dma}
  });
  m.model->refuse = true;
  CHECK(services.write(target, 4, 0, initiator::dma) == std::unexpected{bus_error::denied});
  std::array<std::byte, 8> const bytes{};
  m.model->refuse = false;
  for(bool const watched : {false, true})
  {
    CAPTURE(watched);
    auto const watch = watched
                         ? std::optional{m.f->target->add_watch(
                               0x2000'0000, 4, core::watch_kind::write,
                               [](core::watch_hit const&) { return core::watch_action::resume; })}
                         : std::nullopt;
    auto const block = services.write_block(target, bytes, initiator::dma);
    REQUIRE_FALSE(block);
    CHECK(block.error().error == bus_error::denied);
    CHECK(block.error().at == target);
    CHECK(m.model->stores.size() == 2);
  }
  CHECK(services.write_block(target, bytes, initiator::debugger));
  CHECK(flash_word(m, target) == 0u);
  CHECK(m.model->stores.size() == 2);
}

TEST_CASE("after a restore into the same machine stores still reach the model")
{
  auto       m     = booted();
  auto const saved = m.f->target->snapshot();
  REQUIRE(store(m, "str_entry", target, 1).reason == stop_reason::user);
  m.f->target->restore(saved);
  REQUIRE(store(m, "str_entry", target, 2).reason == stop_reason::user);
  REQUIRE(m.model->stores.size() == 2);
  CHECK(m.model->stores[1].value == 2);
}

TEST_CASE("a restore into a fresh machine keeps stores reaching the new machine's model")
{
  auto       first = booted();
  auto const saved = first.f->target->snapshot();
  auto       other = booted();
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  REQUIRE(store(other, "str_entry", target, 3).reason == stop_reason::user);
  CHECK(
      other.model->stores
      == std::vector<seen_store>{
          {"FLASH", target, 4, 3, initiator::cpu}
  });
  CHECK(first.model->stores.empty());
}

TEST_CASE("a watch sees a DMA store the model took, once")
{
  auto                         m = booted();
  std::vector<core::watch_hit> hits;
  auto const                   watch = m.f->target->add_watch(
      target, 4, core::watch_kind::write, [&hits](core::watch_hit const& hit) {
        hits.push_back(hit);
        return core::watch_action::resume;
      });
  REQUIRE(m.model->services().write(target, 4, 0x5a5a'5a5a, initiator::dma));
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].addr == target);
  CHECK(hits[0].value == 0x5a5a'5a5a);
  CHECK(hits[0].write);
  CHECK(hits[0].by == initiator::dma);
  m.model->refuse = true;
  CHECK_FALSE(m.model->services().write(target, 4, 1, initiator::dma));
  CHECK(hits.size() == 1);
}

TEST_CASE("a region without a controller, or a controller without a model, refuses as before")
{
  {
    auto       m    = booted();
    auto const stop = store(m, "str_entry", target - 0x0800'0000, 1);
    REQUIRE(stop.reason == stop_reason::fault);
    CHECK(stop.fault->fault_address == target - 0x0800'0000);
    CHECK(m.model->stores.empty());
  }
  {
    auto       m    = booted(false);
    auto const stop = store(m, "str_entry", target, 1);
    REQUIRE(stop.reason == stop_reason::fault);
    CHECK(stop.fault->fault_address == target);
  }
}

TEST_CASE("a load overlapping a watched range reaches the controller once, loads beside it do not")
{
  auto m = booted();
  m.model->watch(watched_at, 16);
  CHECK(loads(m, watched_at) == std::vector{cpu_read(watched_at)});
  CHECK(loads(m, watched_at + 12) == std::vector{cpu_read(watched_at + 12)});
  CHECK(loads(m, watched_at - 4).empty());
  CHECK(loads(m, watched_at + 16).empty());
  CHECK(loads(m, watched_at - 3) == std::vector{cpu_read(watched_at - 3)});
  CHECK(loads(m, watched_at + 15) == std::vector{cpu_read(watched_at + 15)});
}

TEST_CASE("a load through an alias reports the alias address and region")
{
  auto       m     = booted();
  auto const alias = watched_at - 0x0800'0000;
  m.model->watch(watched_at, 16);
  CHECK(loads(m, alias) == std::vector{cpu_read(alias, 4, "FLASH_ALIAS")});
  CHECK(loads(m, alias - 4).empty());
}

TEST_CASE("LDRD across the range end reports once, LDM reports each word that overlaps")
{
  auto m = booted();
  m.model->watch(watched_at, 16);
  REQUIRE(load(m, "ldrd_entry", watched_at + 12).reason == stop_reason::user);
  CHECK(m.model->reads == std::vector{cpu_read(watched_at + 12)});
  m.model->reads.clear();
  REQUIRE(load(m, "ldm_entry", watched_at - 4).reason == stop_reason::user);
  CHECK(
      m.model->reads
      == std::vector{cpu_read(watched_at), cpu_read(watched_at + 4), cpu_read(watched_at + 8)});
}

TEST_CASE("a 64-bit load reaching a range from 4 bytes before it reports once with size 8")
{
  auto m = booted();
  m.model->watch(watched_at + 4, 4);
  REQUIRE(load(m, "vldr_entry", watched_at).reason == stop_reason::user);
  CHECK(m.f->reg("r0") == 1);
  CHECK(m.model->reads == std::vector{cpu_read(watched_at, 8)});
  m.model->reads.clear();
  REQUIRE(load(m, "vldr_entry", watched_at + 8).reason == stop_reason::user);
  CHECK(m.model->reads.empty());
}

TEST_CASE("a range watched again by a store after an in-call change reports its next load")
{
  auto m = booted();
  m.model->watch(watched_at, 16);
  m.model->on_load = [](store_model& self) {
    self.on_load = {};
    self.watch(watched_at, 16, false);
    self.watch(target - 0x100, 4);
  };
  m.model->on_store = [](store_model& self) {
    self.on_store = {};
    self.watch(watched_at, 16);
  };
  m.f->set("r0", 0);
  m.f->set("r2", watched_at);
  m.f->set("r4", target + 0x200);
  m.f->set("pc", fn("reread_store_entry"));
  REQUIRE(m.f->run_to_stop({.max_instructions = popcorn::icount{64}}).reason == stop_reason::user);
  CHECK(m.f->reg("r0") == 1);
  CHECK(m.model->stores.size() == 1);
  CHECK(m.model->reads == std::vector{cpu_read(watched_at), cpu_read(watched_at)});
}

TEST_CASE("a 64-bit load reaching the last byte of its span reports, the lead is 7 bytes")
{
  auto m = booted();
  m.model->watch(watched_at + 7, 1);
  REQUIRE(load(m, "vldr_entry", watched_at).reason == stop_reason::user);
  CHECK(m.model->reads == std::vector{cpu_read(watched_at, 8)});
}

TEST_CASE("an unaligned load from the page before reaches a range at the page start, once")
{
  auto m = booted();
  m.model->watch(target, 16);
  CHECK(loads(m, target - 2) == std::vector{cpu_read(target - 2)});
  CHECK(loads(m, target - 4).empty());
  m.model->watch(target - 16, 16);
  CHECK(loads(m, target - 2) == std::vector{cpu_read(target - 2)});
  CHECK(loads(m, target - 4) == std::vector{cpu_read(target - 4)});
  m.model->watch(target, 16, false);
  CHECK(loads(m, target - 2) == std::vector{cpu_read(target - 2)});
  m.model->watch(target - 16, 16, false);
  m.model->watch(target + 1, 1);
  CHECK(loads(m, target - 2) == std::vector{cpu_read(target - 2)});
  CHECK(loads(m, target - 3).empty());
}

TEST_CASE("DMA reads report with their initiator, debugger and host reads never")
{
  auto  m        = booted();
  auto& services = m.model->services();
  m.model->watch(watched_at, 16);
  REQUIRE(services.read(watched_at + 4, 4, initiator::dma));
  CHECK(
      m.model->reads
      == std::vector<seen_read>{
          {"FLASH", watched_at + 4, 4, initiator::dma}
  });
  m.model->reads.clear();
  std::array<std::byte, 8> bytes{};
  REQUIRE(services.read_block(watched_at + 12, bytes, initiator::dma));
  CHECK(
      m.model->reads
      == std::vector<seen_read>{
          {"FLASH", watched_at + 12, 4, initiator::dma}
  });
  m.model->reads.clear();
  REQUIRE(services.read(watched_at - 4, 4, initiator::dma));
  REQUIRE(services.read(watched_at, 4, initiator::debugger));
  REQUIRE(services.read(watched_at, 4, initiator::cpu));
  REQUIRE(services.read_block(watched_at, bytes, initiator::debugger));
  REQUIRE(m.f->target->mem().read(watched_at, 4));
  CHECK(m.model->reads.empty());
}

TEST_CASE(
    "a DMA read of another buffer at a watched offset reports nothing, red only when that buffer "
    "sorts after")
{
  auto  m        = booted();
  auto& services = m.model->services();
  m.model->watch(watched_at, 16);
  std::array<std::byte, 16> bytes{};
  REQUIRE(services.read(0x2000'0000 + (watched_at - 0x0800'0000), 4, initiator::dma));
  REQUIRE(services.read_block(0x2000'0000 + (watched_at - 0x0800'0000), bytes, initiator::dma));
  CHECK(m.model->reads.empty());
}

TEST_CASE("watch_reads off stops reports, twice on and once off stops them")
{
  auto m = booted();
  m.model->watch(watched_at, 16);
  m.model->watch(watched_at, 16, false);
  CHECK(loads(m, watched_at).empty());
  m.model->watch(watched_at, 16);
  m.model->watch(watched_at, 16);
  CHECK(m.f->target->controller_watches() == 1);
  CHECK(loads(m, watched_at).size() == 1);
  m.model->watch(watched_at, 16, false);
  CHECK(loads(m, watched_at).empty());
  CHECK(m.f->target->controller_watches() == 0);
  CHECK(m.f->target->controller_watch_hooks() == 0);
  m.model->watch(watched_at, 16, false);
  CHECK(m.f->target->controller_watches() == 0);
}

TEST_CASE("ranges on one engine page share one hook per alias span")
{
  auto        m        = booted();
  auto const& target_m = *m.f->target;
  m.model->watch(watched_at, 16);
  CHECK(target_m.controller_watch_hooks() == 2);
  m.model->watch(watched_at + 0x20, 16);
  m.model->watch(watched_at + 0x40, 16);
  CHECK(target_m.controller_watches() == 3);
  CHECK(target_m.controller_watch_hooks() == 2);
  CHECK(loads(m, watched_at + 0x18).empty());
  m.model->watch(watched_at + 0x10, 16);
  CHECK(target_m.controller_watches() == 4);
  CHECK(target_m.controller_watch_hooks() == 2);
  CHECK(loads(m, watched_at + 0x18) == std::vector{cpu_read(watched_at + 0x18)});
  CHECK(loads(m, watched_at + 0x1e) == std::vector{cpu_read(watched_at + 0x1e)});
  m.model->watch(watched_at + 0x10, 16, false);
  CHECK(target_m.controller_watch_hooks() == 2);
  CHECK(loads(m, watched_at + 0x18).empty());
  CHECK(loads(m, watched_at + 0x20) == std::vector{cpu_read(watched_at + 0x20)});
  m.model->watch(watched_at + 0x400, 16);
  CHECK(target_m.controller_watch_hooks() == 4);
  for(auto const offset : {0x0u, 0x20u, 0x40u})
  {
    m.model->watch(watched_at + offset, 16, false);
  }
  CHECK(target_m.controller_watch_hooks() == 2);
  CHECK(loads(m, watched_at + 0x400) == std::vector{cpu_read(watched_at + 0x400)});
  m.model->watch(watched_at + 0x400, 16, false);
  CHECK(target_m.controller_watches() == 0);
  CHECK(target_m.controller_watch_hooks() == 0);
  m.model->watch(target + 0x3f8, 16);
  CHECK(target_m.controller_watch_hooks() == 4);
  CHECK(loads(m, target + 0x400) == std::vector{cpu_read(target + 0x400)});
  m.model->watch(target + 0x3f8, 16, false);
  m.model->watch(target, 16);
  CHECK(target_m.controller_watch_hooks() == 4);
}

TEST_CASE("64 ranges on one page cost 2 hooks, apart or adjacent")
{
  auto        m        = booted();
  auto const& target_m = *m.f->target;
  for(address slot = 0; slot < 64; ++slot)
  {
    m.model->watch(target + 8 + 16 * slot, 4);
  }
  CHECK(target_m.controller_watches() == 64);
  CHECK(target_m.controller_watch_hooks() == 2);
  CHECK(loads(m, target + 16 * 63).empty());
  CHECK(loads(m, target + 8 + 16 * 63) == std::vector{cpu_read(target + 8 + 16 * 63)});
  for(address slot = 0; slot < 64; ++slot)
  {
    m.model->watch(target + 8 + 16 * slot, 4, false);
  }
  for(address slot = 1; slot < 64; ++slot)
  {
    m.model->watch(target + 16 * slot, 16);
  }
  CHECK(target_m.controller_watches() == 63);
  CHECK(target_m.controller_watch_hooks() == 2);
  CHECK(loads(m, target + 16 * 63 + 8) == std::vector{cpu_read(target + 16 * 63 + 8)});
  CHECK(loads(m, target + 4).empty());
}

TEST_CASE("a model that changes its watches inside on_memory_read gets one call per load")
{
  struct change
  {
    std::string_view                  name;
    std::function<void(store_model&)> first;
    std::function<void(store_model&)> in_call;
  };
  std::vector<change> const changes{
      {"stop and watch again the range that fired",
       [](store_model& self) { self.watch(watched_at, 16); },
       [](store_model& self) {
         self.watch(watched_at, 16, false);
         self.watch(watched_at, 16);
       }                                                                                                                                                                               },
      {"watch a range whose lead reaches the one that fired",
       [](store_model& self) { self.watch(watched_at, 16); },
       [](store_model& self) { self.watch(watched_at + 0x14, 4); }                                                                                                                     },
      {"stop another range of the same run",
       [](store_model& self) {
         self.watch(watched_at, 16);
         self.watch(watched_at + 0x14, 4);
       },                                                                                                            [](store_model& self) { self.watch(watched_at + 0x14, 4, false); }},
      {"watch a range on the page before",                    [](store_model& self) { self.watch(watched_at, 16); },
       [](store_model& self) { self.watch(target - 0x100, 4); }                                                                                                                        },
  };
  for(auto const& entry : changes)
  {
    CAPTURE(entry.name);
    auto m = booted();
    entry.first(*m.model);
    m.model->on_load = [&entry](store_model& self) {
      self.on_load = {};
      entry.in_call(self);
    };
    CHECK(loads(m, watched_at) == std::vector{cpu_read(watched_at)});
    CHECK(loads(m, watched_at) == std::vector{cpu_read(watched_at)});
  }
}

TEST_CASE("a model that stops its watch from inside on_memory_read sees no later load")
{
  auto m = booted();
  m.model->watch(watched_at, 16);
  m.model->on_load = [](store_model& self) { self.watch(watched_at, 16, false); };
  REQUIRE(load(m, "reread_entry", watched_at).reason == stop_reason::user);
  CHECK(m.f->reg("r0") == 1);
  CHECK(m.model->reads == std::vector{cpu_read(watched_at)});
  CHECK(m.f->target->controller_watches() == 0);
  CHECK(m.f->target->controller_watch_hooks() == 0);
  m.model->on_load = {};
  CHECK(loads(m, watched_at).empty());
}

TEST_CASE("a restore into the same machine keeps the watch table")
{
  auto       m     = booted();
  auto const empty = m.f->target->snapshot();
  m.model->watch(watched_at, 16);
  auto const saved = m.f->target->snapshot();
  m.f->target->restore(saved);
  CHECK(m.f->target->controller_watches() == 1);
  CHECK(m.f->target->controller_watch_hooks() == 2);
  CHECK(loads(m, watched_at) == std::vector{cpu_read(watched_at)});
  m.f->target->restore(empty);
  CHECK(m.f->target->controller_watches() == 1);
  CHECK(loads(m, watched_at) == std::vector{cpu_read(watched_at)});
  m.f->target->reset();
  CHECK(m.f->target->controller_watches() == 1);
}

TEST_CASE("a restore into a fresh machine lets its model rebuild the watch in load()")
{
  auto first = booted();
  first.model->watch(watched_at, 16);
  auto const saved             = first.f->target->snapshot();
  auto       other             = booted();
  other.model->rebuild_on_load = true;
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.f->target->controller_watches() == 1);
  CHECK(loads(other, watched_at) == std::vector{cpu_read(watched_at)});
  CHECK(first.model->reads.empty());
}

TEST_CASE("an interrupt raised from on_memory_read is taken before the next instruction")
{
  auto m = booted();
  REQUIRE(load(m, "enable_irq_entry", 0).reason == stop_reason::user);
  m.model->watch(watched_at, 16);
  m.model->on_load = [](store_model& self) {
    self.context().irqs[0].raise();
    self.context().irqs[0].lower();
  };
  m.f->set("r5", 10);
  m.f->set("r6", 0);
  REQUIRE(load(m, "irq_entry", watched_at).reason == stop_reason::user);
  CHECK(m.model->reads.size() == 1);
  CHECK(m.f->reg("r6") == 10);
  CHECK(m.f->reg("r5") == 11);
  CHECK(m.f->reg("r0") == 1);
}

TEST_CASE("watch_reads refuses a range outside one region with a controller model")
{
  {
    auto  m        = booted();
    auto& services = m.model->services();
    for(auto const& [at, size] : std::initializer_list<std::pair<address, std::uint32_t>>{
            {0x2000'0000,              16},
            {watched_at - 0x0800'0000, 16},
            {0x0820'0000 - 8,          16},
            {0x0c00'0000,              16}
    })
    {
      CAPTURE(at);
      CHECK_THROWS_AS(services.watch_reads(at, size, true), popcorn::config_error);
      CHECK_THROWS_AS(services.watch_reads(at, size, false), popcorn::config_error);
    }
    CHECK(m.f->target->controller_watches() == 0);
    CHECK(m.f->target->controller_watch_hooks() == 0);
    m.model->watch(watched_at, 16);
    CHECK(loads(m, watched_at) == std::vector{cpu_read(watched_at)});
  }
}
