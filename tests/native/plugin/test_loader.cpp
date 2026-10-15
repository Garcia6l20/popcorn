#include "fixture_files.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <doctest/doctest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
using plugin_test::copies;
using plugin_test::fixture;
using plugin_test::init_calls;
using popcorn::plugin::load;
using popcorn::plugin::scope;

auto refusal(std::filesystem::path const& path, scope where = scope::declared) -> std::string
{
  try
  {
    static_cast<void>(load(path, where));
  }
  catch(popcorn::config_error const& error)
  {
    return error.what();
  }
  FAIL("the load was not refused: ", path.string());
  return {};
}

auto replaced_field(std::string tag, std::string_view field, std::string_view value) -> std::string
{
  auto const start = tag.find(std::string{";"} + std::string{field} + " ");
  REQUIRE(start != std::string::npos);
  auto const from = start + field.size() + 2;
  auto const end  = tag.find(';', from);
  tag.replace(from, (end == std::string::npos ? tag.size() : end) - from, value);
  return tag;
}

auto arch_registered(std::string const& cpu) -> bool
{
  popcorn::part_description part;
  part.part     = "plugin-test";
  part.core.cpu = cpu;
  try
  {
    static_cast<void>(popcorn::arch::find_arch(part));
    return true;
  }
  catch(popcorn::config_error const&)
  {
    return false;
  }
}
}

TEST_CASE("a declared plugin loads and records its models, never globally")
{
  copies      files;
  auto const  path   = files.of("good");
  auto const& plugin = load(path, scope::declared);
  CHECK(plugin.name == "good");
  CHECK(plugin.path == std::filesystem::canonical(path));
  CHECK(plugin.abi_tag == popcorn::plugin::host_abi_tag());
  CHECK(plugin.where == scope::declared);
  REQUIRE(plugin.models.size() == 1);
  CHECK(plugin.models.front().name == "fake");
  CHECK(static_cast<bool>(popcorn::plugin::find_model(plugin, "fake")));
  CHECK_FALSE(static_cast<bool>(popcorn::plugin::find_model(plugin, "nope")));
  CHECK_FALSE(popcorn::periph::registered_glob("fake", "1.0"));
}

TEST_CASE("loading a plugin twice returns the first record and runs init once")
{
  copies      files;
  auto const  path  = files.of("good");
  auto const& first = load(path, scope::declared);
  auto const& again = load(path.parent_path() / "." / path.filename(), scope::declared);
  CHECK(&first == &again);
  CHECK(init_calls(path) == 1);
}

TEST_CASE("two threads loading one plugin run its init once")
{
  copies files;
  for(int run = 0; run < 100; ++run)
  {
    auto const                     path = files.of("good");
    std::barrier                   start{2};
    popcorn::plugin::loaded const* seen[2]{};
    auto const                     loads = [&](int slot) {
      start.arrive_and_wait();
      seen[slot] = &load(path, scope::declared);
    };
    std::jthread other{loads, 1};
    loads(0);
    other.join();
    CHECK(seen[0] == seen[1]);
    REQUIRE(init_calls(path) == 1);
  }
}

TEST_CASE("each plugin's own init runs")
{
  copies     files;
  auto const first  = files.of("good");
  auto const second = files.of("good");
  static_cast<void>(load(first, scope::declared));
  static_cast<void>(load(second, scope::declared));
  CHECK(init_calls(first) == 1);
  CHECK(init_calls(second) == 1);
}

TEST_CASE("a model name registered twice by one plugin refuses the load")
{
  copies     files;
  auto const path = files.of("dup_name");
  CHECK(refusal(path).contains("models.fake"));
  CHECK(refusal(path).contains("models.fake"));
}

TEST_CASE("a declared init calling register_model is refused and registers nothing")
{
  copies     files;
  auto const path = files.of("writes_global");
  REQUIRE_FALSE(popcorn::periph::registered_glob("WRITES_GLOBAL", "1.0"));
  auto const message = refusal(path);
  CHECK(message.contains("periph::register_model"));
  CHECK(message.contains(std::filesystem::canonical(path).string()));
  CHECK_FALSE(popcorn::periph::registered_glob("WRITES_GLOBAL", "1.0"));
}

TEST_CASE("a declared init calling register_arch is refused and registers nothing")
{
  copies     files;
  auto const path = files.of("writes_arch");
  REQUIRE_FALSE(arch_registered("writes-arch"));
  CHECK(refusal(path).contains("arch::register_arch"));
  CHECK_FALSE(arch_registered("writes-arch"));
}

TEST_CASE("a builtin init may write the global registry")
{
  copies      files;
  auto const  path   = files.of("writes_global");
  auto const& plugin = load(path, scope::builtin);
  CHECK(plugin.where == scope::builtin);
  CHECK(popcorn::periph::registered_glob("WRITES_GLOBAL", "1.0") == "*");
  popcorn::periph::register_model("WRITES_GLOBAL", "*", {});
}

TEST_CASE("a static constructor calling register_model refuses the load, process alive")
{
  copies     files;
  auto const path = files.of("ctor_registers");
  REQUIRE_FALSE(popcorn::periph::registered_glob("CTOR_REGISTERS", "1.0"));
  auto const message = refusal(path);
  CHECK(message.contains("static constructors"));
  CHECK(message.contains("periph::register_model"));
  CHECK(message.contains(std::filesystem::canonical(path).string()));
  CHECK_FALSE(popcorn::periph::registered_glob("CTOR_REGISTERS", "1.0"));
  CHECK(refusal(path).contains("periph::register_model"));
}

TEST_CASE("an init swallowing the guard's throw is still refused")
{
  copies     files;
  auto const path    = files.of("init_swallows");
  auto const message = refusal(path);
  CHECK(message.contains("popcorn_plugin_init"));
  CHECK(message.contains("periph::register_model"));
  CHECK_FALSE(popcorn::periph::registered_glob("INIT_SWALLOWS", "1.0"));
}

TEST_CASE("plugin::load inside a plugin init is refused")
{
  copies     files;
  auto const nested = files.of("good");
  REQUIRE(setenv("POPCORN_PLUGIN_NESTED", nested.c_str(), 1) == 0);
  CHECK(refusal(files.of("nested_load")).contains("plugin::load inside a plugin init"));
  CHECK_FALSE(plugin_test::is_loaded(nested));
}

TEST_CASE("a plugin without its own init is refused, not served by a library it links")
{
  copies files;
  CHECK(refusal(files.of("no_init_stm32")).contains("popcorn_plugin_init"));
  CHECK(refusal(files.of("no_init")).contains("popcorn_plugin_init"));
}

TEST_CASE("a stripped plugin keeps its tag and loads")
{
  copies      files;
  auto const& plugin = load(files.of("good-stripped"), scope::declared);
  CHECK(plugin.name == "good");
}

TEST_CASE("a plugin built by another compiler is refused naming both tags")
{
  copies     files;
  auto const host    = std::string{popcorn::plugin::host_abi_tag()};
  auto const message = refusal(files.of("mismatch"));
  CHECK(message.contains("abi_tag"));
  CHECK(message.contains(host));
  CHECK(message.contains(replaced_field(host, "gcc", "0.0.0 edited")));
}

TEST_CASE("a plugin built against other SDK headers is refused naming both tags")
{
  copies     files;
  auto const host    = std::string{popcorn::plugin::host_abi_tag()};
  auto const plugin  = replaced_field(host, "sdk", "edited-sdk-digest");
  auto const message = refusal(files.of("sdk_mismatch"));
  CHECK(message.contains(host));
  CHECK(message.contains(plugin));
}

TEST_CASE("a plugin built in libstdc++ debug mode is refused naming both tags")
{
  copies     files;
  auto const host = std::string{popcorn::plugin::host_abi_tag()};
  REQUIRE(host.contains(";glibcxx-debug 0;"));
  auto const message = refusal(files.of("glibcxx_debug"));
  CHECK(message.contains("abi_tag"));
  CHECK(message.contains(host));
  CHECK(message.contains(replaced_field(host, "glibcxx-debug", "1")));
}

TEST_CASE("a mismatched plugin is refused before its static constructors run")
{
  copies files;
  CHECK(refusal(files.of("ctor_aborts")).contains("abi_tag"));
}

TEST_CASE("an init that throws records nothing and throws again on the next load")
{
  copies     files;
  auto const path = files.of("throws");
  CHECK_THROWS_WITH_AS(
      load(path, scope::declared), "throws fixture: init failed", std::runtime_error);
  CHECK_THROWS_WITH_AS(
      load(path, scope::declared), "throws fixture: init failed", std::runtime_error);
}

TEST_CASE("a library that is not a popcorn plugin is refused")
{
  auto const core = std::filesystem::path{std::getenv("POPCORN_PLUGIN_CORE")};
  CHECK(refusal(core).contains(".popcorn_abi"));
  CHECK(refusal(plugin_test::fixtures() / "missing.so").contains("path"));
}

TEST_CASE("a hard link to a plugin refused for its static constructors is refused")
{
  copies     files;
  auto const path = files.of("ctor_registers");
  CHECK(refusal(path).contains("static constructors"));
  auto const link = files.hard_link(path);
  CHECK(refusal(link).contains("static constructors"));
  CHECK_FALSE(popcorn::periph::registered_glob("CTOR_REGISTERS", "1.0"));
}

TEST_CASE("two hard links to one plugin give one record and run its init once")
{
  copies      files;
  auto const  path   = files.of("good");
  auto const  link   = files.hard_link(path);
  auto const& first  = load(path, scope::declared);
  auto const& second = load(link, scope::declared);
  CHECK(&first == &second);
  CHECK(init_calls(path) == 1);
}

TEST_CASE("a plugin mapped into the process before plugin::load is refused")
{
  copies      files;
  auto const  path   = files.of("good");
  auto* const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  REQUIRE(handle != nullptr);
  CHECK(refusal(path).contains("mapped before"));
  CHECK(init_calls(path) == 0);
  dlclose(handle);
}

TEST_CASE("a builtin load that failed after dlopen leaves a declared load refused as mapped before")
{
  copies     files;
  auto const path = files.of("throws");
  CHECK_THROWS_WITH_AS(
      load(path, scope::builtin), "throws fixture: init failed", std::runtime_error);
  CHECK(refusal(path).contains("mapped before"));
}

TEST_CASE("a plugin loaded with one scope is refused with the other")
{
  copies     files;
  auto const declared = files.of("good");
  static_cast<void>(load(declared, scope::declared));
  CHECK(refusal(declared, scope::builtin).contains("scope"));
  auto const builtin = files.of("good");
  static_cast<void>(load(builtin, scope::builtin));
  CHECK(refusal(builtin, scope::declared).contains("scope"));
  CHECK(load(builtin, scope::builtin).where == scope::builtin);
}

TEST_CASE("the copies directory is removed with its object")
{
  std::filesystem::path dir;
  {
    copies files;
    static_cast<void>(files.of("good"));
    dir = files.dir();
    REQUIRE(std::filesystem::exists(dir));
  }
  CHECK_FALSE(std::filesystem::exists(dir));
}

TEST_CASE("a dead process's copies directory is swept by the next copies")
{
  auto const child = fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    _exit(0);
  }
  REQUIRE(waitpid(child, nullptr, 0) == child);
  auto const stale = std::filesystem::temp_directory_path()
                   / std::format("{}{}-0", plugin_test::copies_prefix, child);
  std::filesystem::create_directories(stale);
  {
    copies files;
  }
  CHECK_FALSE(std::filesystem::exists(stale));
}
