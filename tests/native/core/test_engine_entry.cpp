#include "../../../native/src/core/machine_impl.hpp"

#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;
using fixture = popcorn::testing::machine_fixture<>;

auto booted(std::string_view firmware = "intercepts") -> fixture
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  fixture f{m33(), firmware, options};
  std::ignore = f.run_to_stop();
  return f;
}

auto fn(std::string_view name) -> address
{
  return sym("intercepts", name);
}

auto depth(fixture const& f) -> std::uint32_t
{
  return core::machine_internals::entry_depth(*f.target);
}

auto read_text(std::filesystem::path const& path) -> std::string
{
  std::ifstream     in{path};
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

auto sources() -> std::map<std::string, std::string>
{
  auto const* const root = std::getenv("POPCORN_NATIVE_SRC");
  REQUIRE(root != nullptr);
  std::map<std::string, std::string> found;
  for(auto const& entry : std::filesystem::recursive_directory_iterator{root})
  {
    auto const extension = entry.path().extension();
    if(entry.is_regular_file() and (extension == ".cpp" or extension == ".hpp"))
    {
      found.emplace(
          std::filesystem::relative(entry.path(), root).generic_string(), read_text(entry.path()));
    }
  }
  return found;
}

auto registrations(std::string const& text) -> std::size_t
{
  static std::regex const call{
      R"(\b(uc_hook_add|uc_mmio_map\w*|uc_ctl_bus_filter|uc_ctl_arm_idau)\s*\()"};
  return static_cast<std::size_t>(
      std::distance(std::sregex_iterator{text.begin(), text.end(), call}, std::sregex_iterator{}));
}

auto body_of(std::string const& text, std::string_view function) -> std::string
{
  auto const at = text.find(function);
  if(at == std::string::npos)
  {
    return {};
  }
  auto const  open  = text.find('{', at);
  std::size_t depth = 0;
  for(auto end = open; end < text.size(); ++end)
  {
    depth += text[end] == '{' ? 1 : 0;
    depth -= text[end] == '}' ? 1 : 0;
    if(depth == 0)
    {
      return text.substr(open, end + 1 - open);
    }
  }
  return {};
}

auto opens_with(std::string const& text, std::string_view function, std::string_view guard) -> bool
{
  auto const at = text.find(function);
  if(at == std::string::npos)
  {
    return false;
  }
  auto const open   = text.find('{', at);
  auto const second = text.find(';', text.find(';', open) + 1);
  return text.substr(open, second - open).contains(guard);
}

}

TEST_CASE("intercept and watch callbacks run inside one engine entry, none between runs")
{
  auto f = booted();
  CHECK(depth(f) == 0);

  std::vector<std::uint32_t> seen;
  auto const intercept = f.target->add_intercept(fn("delay_ms"), [&](core::intercept_call const&) {
    seen.push_back(depth(f));
    return core::intercept_result::returns(
        popcorn::arch::call_arg{.kind = popcorn::arch::value_kind::i32, .bits = 5});
  });
  auto const watch =
      f.target->add_watch(fn("counter"), 4, core::watch_kind::write, [&](core::watch_hit const&) {
        seen.push_back(depth(f));
        return core::watch_action::resume;
      });
  std::ignore = f.target->call(
      {.entry           = fn("use_delay"),
       .args            = {},
       .returns         = popcorn::arch::value_kind::i32,
       .abi             = popcorn::arch::float_abi::soft,
       .return_sentinel = 0x3000'0000},
      icount{10'000'000});
  REQUIRE(seen.size() == 1);
  f.set("pc", fn("bump"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(seen.size() == 2);
  CHECK(seen == std::vector<std::uint32_t>{1, 1});
  CHECK(depth(f) == 0);
}

TEST_CASE("a model write from firmware runs inside one engine entry")
{
  auto f = booted("periph");
  CHECK(depth(f) == 0);
  std::vector<std::uint32_t> seen;
  auto&                      model = dynamic_cast<test_model&>(*f.target->model("TESTDEV"));
  model.write_probe                = [&] { seen.push_back(depth(f)); };
  f.set("pc", sym("periph", "model_access"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(seen == std::vector<std::uint32_t>{1});
  CHECK(depth(f) == 0);
}

TEST_CASE("every engine callback is registered through a guarded entry")
{
  auto const                         files = sources();
  std::map<std::string, std::size_t> calls;
  for(auto const& [name, text] : files)
  {
    if(auto const count = registrations(text); count != 0)
    {
      calls.emplace(name, count);
    }
  }
  CHECK(
      calls
      == std::map<std::string, std::size_t>{
          {"arch/armv8m/cortex_m.cpp", 2},
          {"arch/armv8m/security.cpp", 1},
          {"core/engine.cpp",          1},
          {"core/machine_impl.hpp",    4},
  });

  auto const& helpers = files.at("core/machine_impl.hpp");
  CHECK(helpers.contains("&entry_thunk<callback, User>::call"));
  CHECK(helpers.contains("&entry_thunk<read, User>::call"));
  CHECK(helpers.contains("&entry_thunk<write, User>::call"));
  CHECK(helpers.contains("&entry_thunk<page, run_state>::call"));
  CHECK(helpers.contains("&entry_thunk<access, run_state>::call"));

  auto const& arch  = files.at("arch/armv8m/cortex_m.cpp");
  auto const& sleep = files.at("arch/armv8m/sleep.cpp");
  CHECK(arch.contains("&mmio_read, &user, &mmio_write, &user"));
  CHECK(arch.contains("reinterpret_cast<void*>(&wfi_hook)"));
  CHECK(opens_with(arch, "auto cortex_m::mmio_read(", "engine_callback const"));
  CHECK(opens_with(arch, "void cortex_m::mmio_write(", "engine_callback const"));
  CHECK(opens_with(sleep, "auto cortex_m::wfi_hook(", "engine_callback const"));

  auto const& security = files.at("arch/armv8m/security.cpp");
  CHECK(security.contains("uc_ctl_arm_idau(engine, &idau_lookup, this)"));
  auto const idau = body_of(security, "void cortex_m::idau_lookup(");
  REQUIRE_FALSE(idau.empty());
  CHECK(idau.contains("context_->idau"));
  for(auto const word : {"clock", "services", "pending", "events", "state_"})
  {
    CAPTURE(word);
    CHECK_FALSE(idau.contains(word));
  }

  CHECK(helpers.contains("UC_HOOK_BLOCK_ICOUNT, reinterpret_cast<void*>(&on_block), this, 1, 0)"));
  std::size_t block_hooks = 0;
  for(auto const& text : files | std::views::values)
  {
    for(auto at = text.find("add_block_hook("); at != std::string::npos;
        at      = text.find("add_block_hook(", at + 1))
    {
      ++block_hooks;
    }
  }
  CHECK(block_hooks == 2);

  auto const& engine = files.at("core/engine.cpp");
  CHECK(engine.contains(
      "void on_access(uc_struct*, uc_mem_type, std::uint64_t, int, std::int64_t, void*) "
      "noexcept\n{}"));
}

TEST_CASE("the services engine callback scope enters the machine's engine entry and nests")
{
  auto  f        = booted();
  auto& services = core::machine_internals::services(*f.target);
  {
    core::machine_services::engine_callback const outer{services};
    CHECK(depth(f) == 1);
    {
      core::machine_services::engine_callback const inner{services};
      CHECK(depth(f) == 2);
    }
    CHECK(depth(f) == 1);
  }
  CHECK(depth(f) == 0);
}
