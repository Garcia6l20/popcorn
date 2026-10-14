#include <popcorn/periph/i2c.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/uart.hpp>

#include <doctest/doctest.h>

#include <dlfcn.h>

#include <filesystem>
#include <string>
#include <typeinfo>

namespace
{
namespace periph = popcorn::periph;

auto library_of(std::type_info const& type) -> std::string
{
  Dl_info info{};
  REQUIRE(dladdr(&type, &info) != 0);
  REQUIRE(info.dli_fname != nullptr);
  return std::filesystem::path{info.dli_fname}.filename().string();
}
}

TEST_CASE("the SPI interfaces' type info lives in libpopcorn_core.so")
{
  CHECK(library_of(typeid(periph::spi_target)) == "libpopcorn_core.so");
  CHECK(library_of(typeid(periph::spi_controller_sink)) == "libpopcorn_core.so");
  CHECK(library_of(typeid(periph::spi_controller_port)) == "libpopcorn_core.so");
}

TEST_CASE("the I2C target's type info lives in libpopcorn_core.so")
{
  CHECK(library_of(typeid(periph::i2c_target)) == "libpopcorn_core.so");
}

TEST_CASE("the UART interfaces' type info lives in libpopcorn_core.so")
{
  CHECK(library_of(typeid(periph::uart_endpoint)) == "libpopcorn_core.so");
  CHECK(library_of(typeid(periph::uart_port)) == "libpopcorn_core.so");
  CHECK(library_of(typeid(periph::capture_endpoint)) == "libpopcorn_core.so");
}
