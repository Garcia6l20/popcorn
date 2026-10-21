#include <popcorn/arch/arch.hpp>
#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace
{
using popcorn::config_error;
using popcorn::core_desc;
using popcorn::part_description;
namespace arch = popcorn::arch;

constexpr int no_tag = -1;

int called_tag = no_tag;

struct fake_factory
{
  int tag;

  auto operator()(core_desc const&) const -> std::unique_ptr<arch::arch>
  {
    called_tag = tag;
    return nullptr;
  }
};

auto called_with(arch::arch_factory factory, core_desc const& core) -> int
{
  called_tag = no_tag;
  try
  {
    std::ignore = factory(core);
  }
  catch(...)
  {
    return no_tag;
  }
  return called_tag;
}

auto part_with(std::string cpu) -> part_description
{
  part_description description;
  description.part     = "FAKE-PART";
  description.core.cpu = std::move(cpu);
  return description;
}

void register_fake(std::string name, std::string cpu, int tag)
{
  arch::register_arch(
      {.name       = std::move(name),
       .implements = [cpu = std::move(cpu)](core_desc const& core) { return core.cpu == cpu; },
       .factory    = fake_factory{tag}});
}

auto tag_of(std::string cpu) -> int
{
  auto const part = part_with(std::move(cpu));
  auto const tag  = called_with(arch::find_arch(part), part.core);
  REQUIRE(tag != no_tag);
  return tag;
}

auto refused(std::string cpu) -> bool
{
  try
  {
    static_cast<void>(arch::find_arch(part_with(std::move(cpu))));
  }
  catch(config_error const&)
  {
    return true;
  }
  return false;
}
}

TEST_CASE("a core no module implements is a config_error naming core.cpu")
{
  try
  {
    static_cast<void>(arch::find_arch(part_with("CM0")));
    FAIL("find_arch returned for CM0");
  }
  catch(config_error const& error)
  {
    CHECK(error.file() == "FAKE-PART");
    CHECK(error.line() == 0);
    CHECK(error.key() == "core.cpu");
    CHECK(error.expected() == "a core an arch module implements, not CM0");
  }
}

TEST_CASE("a module is found only for the cores it implements")
{
  register_fake("only", "FAKE-ONLY", 1);
  CHECK(tag_of("FAKE-ONLY") == 1);
  CHECK(refused("FAKE-ONLY-NOT"));
}

TEST_CASE("the later of two modules implementing one core wins")
{
  register_fake("later-a", "FAKE-LATER", 1);
  register_fake("later-b", "FAKE-LATER", 2);
  CHECK(tag_of("FAKE-LATER") == 2);
}

TEST_CASE("registering a name again replaces the module")
{
  register_fake("replaced", "FAKE-REPLACED-OLD", 1);
  register_fake("replaced", "FAKE-REPLACED-NEW", 2);
  CHECK(refused("FAKE-REPLACED-OLD"));
  CHECK(tag_of("FAKE-REPLACED-NEW") == 2);
}

TEST_CASE("a replaced module counts as the latest registered")
{
  register_fake("moved-a", "FAKE-MOVED", 1);
  register_fake("moved-b", "FAKE-MOVED", 2);
  register_fake("moved-a", "FAKE-MOVED", 3);
  CHECK(tag_of("FAKE-MOVED") == 3);
}

TEST_CASE("register_builtin registers armv8m once")
{
  arch::armv8m::register_builtin();
  auto const cm33    = part_with("CM33");
  auto       builtin = arch::find_arch(cm33);
  CHECK(static_cast<bool>(builtin));
  CHECK(called_with(std::move(builtin), cm33.core) == no_tag);
  CHECK(refused("CM0"));
  register_fake("shadow-cm33", "CM33", 5);
  arch::armv8m::register_builtin();
  CHECK(tag_of("CM33") == 5);
}
