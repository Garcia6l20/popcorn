#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::test_model;

class line_log_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  std::vector<std::pair<std::uint32_t, bool>> levels;

  void set_irq_line(std::uint32_t source, bool active) override
  {
    levels.emplace_back(source, active);
    test_arch::set_irq_line(source, active);
  }
};

auto with_test_model() -> core::machine_options
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  return options;
}

using fixture = popcorn::testing::machine_fixture<line_log_arch>;

auto booted() -> fixture
{
  fixture f{m33(), "calls", with_test_model()};
  REQUIRE(f.run_to_stop({.budget_left = icount{1000}}).reason == stop_reason::budget);
  return f;
}

auto testdev(fixture const& f) -> test_model&
{
  auto* const found = dynamic_cast<test_model*>(f.target->model("TESTDEV"));
  REQUIRE(found != nullptr);
  return *found;
}

class counting_model final : public periph::model
{
public:
  explicit counting_model(std::string tag) : tag{std::move(tag)}
  {}

  std::string tag;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

auto tagged(std::string tag) -> periph::model_factory
{
  return [tag = std::move(tag)](periph::model_context&, popcorn::peripheral_desc const&)
             -> std::shared_ptr<periph::model> { return std::make_shared<counting_model>(tag); };
}

auto tag_of(core::machine& target, std::string_view name) -> std::string
{
  auto const* const found = dynamic_cast<counting_model*>(target.model(name));
  return found == nullptr ? std::string{} : found->tag;
}
}

TEST_CASE("the registry picks the latest matching registration, explicit wiring beats it")
{
  periph::register_model("testdev", "v1*", tagged("first"));
  periph::register_model("testdev", "v?", tagged("glob"));
  core::machine plain{m33(), popcorn::testing::test_arch_factory(), {}};
  CHECK(tag_of(plain, "TESTDEV") == "glob");
  periph::register_model("testdev", "v1*", tagged("second"));
  core::machine later{m33(), popcorn::testing::test_arch_factory(), {}};
  CHECK(tag_of(later, "TESTDEV") == "second");
  core::machine explicit_wiring{m33(), popcorn::testing::test_arch_factory(), with_test_model()};
  CHECK(dynamic_cast<test_model*>(explicit_wiring.model("TESTDEV")) != nullptr);
  periph::register_model("testdev", "v1*", {});
  periph::register_model("testdev", "v?", {});
}

TEST_CASE("attach resolves names, aliases and models")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), with_test_model()};
  CHECK(target.model("UNMODELLED") == nullptr);
  CHECK(&target.peripheral("SEC_TESTDEV") == &target.peripheral("TESTDEV"));
  CHECK(target.peripheral("SEC_TESTDEV").description().name == "TESTDEV");
  CHECK(target.model("SEC_TESTDEV") == target.model("TESTDEV"));
  CHECK_THROWS_AS(std::ignore = target.peripheral("NOPE"), popcorn::config_error);
  CHECK_THROWS_AS(std::ignore = target.model("NOPE"), popcorn::config_error);
}

TEST_CASE("a claim of a register the peripheral lacks is a configuration error")
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  auto  desc      = m33();
  auto& registers = desc.peripherals.front().registers;
  std::erase_if(registers, [](popcorn::register_desc const& reg) { return reg.name == "CTRL"; });
  try
  {
    core::machine target{desc, popcorn::testing::test_arch_factory(), options};
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& e)
  {
    CHECK(std::string{e.what()}.contains("CTRL"));
  }
}

TEST_CASE("unclaimed registers come out of reset with their description values")
{
  core::machine target{m33(), popcorn::testing::test_arch_factory(), with_test_model()};
  target.reset();
  auto& regs = target.peripheral("TESTDEV");
  CHECK(regs.peek(0x00) == 0x1234'5678);
  CHECK(regs.peek(0x14) == 0x5a);
  CHECK(regs.peek(0x54) == 1);
  CHECK(target.peripheral("UNMODELLED").peek(4) == 1);
}

TEST_CASE("a model's event raises its line once, a read lowers it")
{
  auto  f     = booted();
  auto& model = testdev(f);
  auto& regs  = f.target->peripheral("TESTDEV");
  f.module->levels.clear();
  REQUIRE(model.on_write(regs, test_model::ctrl_offset, 4, 0, 1, {.pc = 0, .now = icount{0}}));
  REQUIRE(model.on_write(regs, test_model::ctrl_offset, 4, 1, 1, {.pc = 0, .now = icount{0}}));
  f.set("primask", 1);
  REQUIRE(
      f.run_to_stop({.budget_left = icount{2 * test_model::delay + 100}}).reason
      == stop_reason::budget);
  CHECK(
      f.module->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {3, true}
  });
  CHECK(f.module->pending());
  REQUIRE(model.on_read(regs, test_model::data_offset, 4, {.pc = 0, .now = icount{0}}));
  CHECK(
      f.module->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {3, true },
          {3, false}
  });
}

TEST_CASE("model context lines are the slot's lines, used in place")
{
  auto  f     = booted();
  auto& model = testdev(f);
  CHECK(model.context().irqs.size() == 1);
  CHECK(model.context().dma.empty());
  CHECK(&model.context().regs == &f.target->peripheral("TESTDEV"));
  f.module->levels.clear();
  model.context().irqs.front().raise();
  CHECK(
      f.module->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {3, true}
  });
}

TEST_CASE("the arch resets before the models, so a line raised in a model reset stays pending")
{
  auto  f              = booted();
  auto& model          = testdev(f);
  model.raise_in_reset = true;
  model.context().irqs.front().raise();
  for(auto const kind : {popcorn::reset_kind::pin, popcorn::reset_kind::software})
  {
    f.module->levels.clear();
    f.target->reset(kind);
    CHECK(model.last_reset == kind);
    CHECK(model.context().irqs.front().level());
    CHECK(
        f.module->levels
        == std::vector<std::pair<std::uint32_t, bool>>{
            {3, true}
    });
    CHECK(f.module->pending());
  }
}

TEST_CASE("a cross rule into a register a model claims is a configuration error")
{
  auto desc = m33();
  desc.peripherals.front().cross_rules.push_back(
      {.reg          = "ICR",
       .field        = "CF",
       .target_reg   = "DATA",
       .target_field = "VAL",
       .effect       = popcorn::cross_effect::set});
  try
  {
    core::machine target{desc, popcorn::testing::test_arch_factory(), with_test_model()};
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& e)
  {
    CHECK(std::string{e.what()}.contains("DATA"));
    CHECK(std::string{e.what()}.contains("TESTDEV"));
  }
}

TEST_CASE("a cross rule between two registers the model claims is the model's")
{
  auto desc = m33();
  desc.peripherals.front().cross_rules.push_back(
      {.reg          = "CTRL",
       .field        = "GO",
       .target_reg   = "DATA",
       .target_field = "VAL",
       .effect       = popcorn::cross_effect::set});
  CHECK_NOTHROW(core::machine{desc, popcorn::testing::test_arch_factory(), with_test_model()});
}

TEST_CASE("two peripherals on one interrupt source drive it wired-OR")
{
  auto desc   = m33();
  auto second = desc.peripherals.front();
  second.name = "TESTDEV2";
  second.base = 0x4001'1000;
  desc.peripherals.push_back(second);
  core::machine_options options = with_test_model();
  options.models.emplace("TESTDEV2", popcorn::testing::test_model_factory());
  fixture f{desc, "calls", options};
  auto& first_line = dynamic_cast<test_model&>(*f.target->model("TESTDEV")).context().irqs.front();
  auto& second_line =
      dynamic_cast<test_model&>(*f.target->model("TESTDEV2")).context().irqs.front();
  f.module->levels.clear();
  first_line.raise();
  second_line.raise();
  first_line.lower();
  CHECK(
      f.module->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {3, true}
  });
  second_line.lower();
  CHECK(
      f.module->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {3, true },
          {3, false}
  });
  second_line.raise();
  f.target->reset();
  f.module->levels.clear();
  first_line.raise();
  CHECK(
      f.module->levels
      == std::vector<std::pair<std::uint32_t, bool>>{
          {3, true}
  });
}

namespace
{
class peer_probe final : public periph::model
{
public:
  explicit peer_probe(periph::model_context& ctx)
      : context{ctx}, in_factory{ctx.peers.find("TESTDEV")}
  {}

  periph::model_context& context;
  periph::model*         in_factory;
  mutable periph::model* in_claims{};
  periph::model*         in_reset{};

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    in_claims = context.peers.find("TESTDEV");
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {
    in_reset = context.peers.find("TESTDEV");
  }
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};
}

TEST_CASE("peers are empty while models are built and name every model after")
{
  core::machine_options options = with_test_model();
  options.models.emplace(
      "UNMODELLED",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<peer_probe>(ctx); });
  core::machine target{m33(), popcorn::testing::test_arch_factory(), options};
  auto&         probe = dynamic_cast<peer_probe&>(*target.model("UNMODELLED"));
  CHECK(probe.in_factory == nullptr);
  CHECK(probe.in_claims == nullptr);
  target.reset();
  CHECK(probe.in_reset == target.model("TESTDEV"));
  CHECK(probe.context.peers.find("SEC_TESTDEV") == target.model("TESTDEV"));
  CHECK(probe.context.peers.find("UNMODELLED") == &probe);
  CHECK(probe.context.peers.find("NOPE") == nullptr);
  CHECK(&probe.context.part == &target.description());
  auto const all = probe.context.peers.all();
  REQUIRE(all.size() == 2);
  CHECK(all[0] == target.model("TESTDEV"));
  CHECK(all[1] == &probe);
}

TEST_CASE("the model context carries strict_peripherals, unchanged by a restore without reset")
{
  for(bool const strict : {false, true})
  {
    CAPTURE(strict);
    core::machine_options options = with_test_model();
    options.strict_peripherals    = strict;
    options.models.emplace(
        "UNMODELLED",
        [](periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> { return std::make_shared<peer_probe>(ctx); });
    core::machine target{m33(), popcorn::testing::test_arch_factory(), options};
    auto&         probe = dynamic_cast<peer_probe&>(*target.model("UNMODELLED"));
    CHECK(probe.context.strict_peripherals == strict);
    auto const saved = target.snapshot();
    target.restore(saved);
    CHECK(target.model("UNMODELLED") == &probe);
    CHECK(probe.context.strict_peripherals == strict);
  }
}
