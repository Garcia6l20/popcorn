#include "arch_fixture.hpp"
#include "cortex_m.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/model.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return ctx_.services;
  }

  std::vector<seen_store> stores{};
  bool                    refuse        = false;
  bool                    write_through = false;

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
