#include <popcorn/config/project.hpp>
#include <popcorn/core/errors.hpp>

#include <doctest/doctest.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace
{
namespace config = popcorn::config;
namespace fs     = std::filesystem;

auto data(std::string_view name) -> fs::path
{
  char const* const root = std::getenv("POPCORN_API_DATA");
  REQUIRE(root != nullptr);
  return fs::path{root} / name;
}

class scratch_file
{
public:
  explicit scratch_file(std::string const& text)
      : path_{
            fs::temp_directory_path()
            / std::format("popcorn-project-{}-{}.toml", ::getpid(), counter_++)}
  {
    std::ofstream{path_} << text;
  }
  scratch_file(scratch_file const&)                    = delete;
  auto operator=(scratch_file const&) -> scratch_file& = delete;
  ~scratch_file()
  {
    std::error_code ignored;
    fs::remove(path_, ignored);
  }

  [[nodiscard]] auto path() const -> fs::path const&
  {
    return path_;
  }

private:
  static inline int counter_ = 0;
  fs::path          path_;
};

auto failure(fs::path const& file, std::optional<std::string_view> name = {})
    -> std::optional<popcorn::config_error>
{
  try
  {
    std::ignore = config::load_project(file, name);
  }
  catch(popcorn::config_error const& e)
  {
    return e;
  }
  return std::nullopt;
}

auto failure_of(std::string const& text) -> popcorn::config_error
{
  scratch_file const file{text};
  auto const         error = failure(file.path());
  REQUIRE(error.has_value());
  CHECK(error->file() == fs::absolute(file.path()).lexically_normal().string());
  return *error;
}

constexpr std::string_view minimal = "[mcu]\npart = \"X\"\n";
}

TEST_CASE("every section of the full project reads back")
{
  auto const  loaded = config::load_project(data("full.toml"));
  auto const  base   = fs::absolute(data("")).lexically_normal();
  auto const& file   = loaded.file;
  CHECK(file.project.name == "demo");
  CHECK(file.mcu.part == "POPCORN-M33-TEST");
  CHECK(file.mcu.svd == "auto");
  CHECK(file.mcu.ioc == (base / "cubemx/demo.ioc").string());
  CHECK(file.mcu.description == (base / "../core/m33.json").lexically_normal().string());
  CHECK(file.image_order == std::vector<std::string>{"app", "boot"});
  CHECK(file.image.at("boot").at == 0x2000'0100u);
  CHECK(file.image.at("boot").bin == (base / "blob.bin").string());
  CHECK(file.boot.vtor == 0x0800'0000u);
  CHECK(file.boot.until == "alu_mark");
  CHECK(file.boot.budget == 1'000'000u);
  REQUIRE(file.boot.memory_init.size() == 1);
  CHECK(file.boot.memory_init[0].symbol == "spin_flag");
  CHECK(file.clock.core_hz == 8'000'000u);
  CHECK(file.clock.cpi == 1.0);
  CHECK(file.board.at("UART4").connect == "pty");
  REQUIRE(file.board.at("I2C1").devices.size() == 1);
  auto const& device = file.board.at("I2C1").devices[0];
  CHECK(device.address == 0x50u);
  CHECK(device.model == "fake_models:Eeprom");
  CHECK(device.params.at("size").as<std::int64_t>() == 8192);
  CHECK(file.board.at("TIM2").model == "fake_models:Timer");
  CHECK(file.board.at("TIM2").params.at("prescaler").as<std::int64_t>() == 4);
  CHECK(file.models.strict);
  CHECK(file.models.python == std::vector<std::string>{(base / "models").string()});
  CHECK(loaded.configs == std::vector<std::string>{"broken", "variant"});
  CHECK_FALSE(loaded.config.has_value());
  CHECK(loaded.digest.size() == 64);
}

TEST_CASE("a config overrides keys and changes the digest")
{
  auto const plain   = config::load_project(data("full.toml"));
  auto const variant = config::load_project(data("full.toml"), "variant");
  auto const base    = fs::absolute(data("")).lexically_normal();
  CHECK(variant.config == "variant");
  CHECK(variant.file.mcu.ioc == (base / "cubemx/variant.ioc").string());
  CHECK(
      variant.file.image.at("app").elf
      == (base / "../../firmware/core/prebuilt/basic.elf").lexically_normal().string());
  CHECK(variant.digest != plain.digest);
}

TEST_CASE("an unknown config and an unknown config key name the config")
{
  auto const unknown = failure(data("full.toml"), "absent");
  REQUIRE(unknown.has_value());
  CHECK(unknown->key() == "configs.absent");
  CHECK(unknown->expected().find("broken, variant") != std::string::npos);

  auto const broken = failure(data("full.toml"), "broken");
  REQUIRE(broken.has_value());
  CHECK(broken->key() == "configs.broken.image.app.elff");
  CHECK(broken->line() == 47);
}

TEST_CASE("an unknown key names its key, line and the keys of its table")
{
  auto const error = failure_of(std::string{minimal} + "\n[clock]\ncore_hz = 1\nspeed = 2\n");
  CHECK(error.key() == "clock.speed");
  CHECK(error.line() == 6);
  CHECK(error.expected() == "a key of clock: core_hz, cpi");

  auto const top = failure_of(std::string{minimal} + "[extra]\n");
  CHECK(top.key() == "extra");
  CHECK(top.line() == 3);
}

TEST_CASE("a wrong type names its key and line")
{
  auto const error = failure_of(std::string{minimal} + "[boot]\nbudget = \"lots\"\n");
  CHECK(error.key() == "boot.budget");
  CHECK(error.line() == 4);
  CHECK(error.expected().starts_with("an integer from 0 to"));

  auto const negative = failure_of(std::string{minimal} + "[boot]\nvtor = -4\n");
  CHECK(negative.key() == "boot.vtor");

  auto const nested = failure_of(
      std::string{minimal}
      + "[board.I2C1]\ndevices = [\n  { address = \"a\", model = \"m:C\" },\n]\n");
  CHECK(nested.key() == "board.I2C1.devices[0].address");
  CHECK(nested.line() == 4);
}

TEST_CASE("a missing required key is an error, the table absent too")
{
  auto const missing = failure_of("[mcu]\nioc = \"a.ioc\"\n");
  CHECK(missing.key() == "mcu.part");
  CHECK(missing.line() == 1);

  auto const absent = failure_of("[project]\nname = \"x\"\n");
  CHECK(absent.key() == "mcu.part");
  CHECK(absent.expected() == "a value, required");
}

TEST_CASE("an image names exactly one file, and at goes with bin")
{
  CHECK(failure_of(std::string{minimal} + "[image.a]\n").key() == "image.a");
  CHECK(
      failure_of(std::string{minimal} + "[image.a]\nelf = \"a\"\nhex = \"b\"\n").key()
      == "image.a");
  CHECK(failure_of(std::string{minimal} + "[image.a]\nbin = \"a\"\n").key() == "image.a.at");
  CHECK(
      failure_of(
          std::string{minimal} + "[boot]\nmemory_init = [{ symbol = \"s\", bytes = \"0\" }]\n")
          .key()
      == "boot.memory_init[0].bytes");
}

TEST_CASE("TOML syntax errors and unreadable files are config errors")
{
  CHECK(failure_of("[mcu\n").key() == "<document>");
  auto const absent = failure(data("absent.toml"));
  REQUIRE(absent.has_value());
  CHECK(absent->key() == "<file>");
}

TEST_CASE("images load in declaration order, not key order")
{
  scratch_file const file{
      std::string{minimal} + "[image.zeta]\nelf = \"z.elf\"\n[image.alpha]\nelf = \"a.elf\"\n"};
  CHECK(
      config::load_project(file.path()).file.image_order
      == std::vector<std::string>{"zeta", "alpha"});
}

TEST_CASE("the digest ignores whitespace and comments and follows every value")
{
  scratch_file const a{std::string{minimal} + "[board.UART4]\nconnect = \"pty\"\n"};
  scratch_file const b{
      "# a comment\n[mcu]\n  part   =  \"X\"   \n\n[board.UART4]\nconnect=\"pty\"\n"};
  scratch_file const c{std::string{minimal} + "[board.UART4]\nconnect = \"tcp\"\n"};
  auto const digest = [](scratch_file const& f) { return config::load_project(f.path()).digest; };
  CHECK(digest(a) == digest(b));
  CHECK(digest(a) != digest(c));
}

TEST_CASE("an error in the second array table entry names its own line")
{
  auto const error = failure_of(
      std::string{minimal}
      + "[[boot.memory_init]]\nsymbol = \"a\"\nbytes = \"00\"\n"
        "[[boot.memory_init]]\nsymbol = \"b\"\nbytes = \"0\"\n");
  CHECK(error.key() == "boot.memory_init[1].bytes");
  CHECK(error.line() == 8);
}

TEST_CASE("a multi-line string does not open tables or keys")
{
  scratch_file const file{
      "[project]\nname = \"\"\"\n[image.x]\nelf = \"x.elf\"\n\"\"\"\n"
      + std::string{minimal}
      + "[image.app]\nelf = \"a.elf\"\n"};
  auto const loaded = config::load_project(file.path());
  CHECK(loaded.file.image_order == std::vector<std::string>{"app"});
  CHECK(loaded.line_of("image.app.elf") == 9);
  CHECK(loaded.line_of("mcu.part") == 7);
}

TEST_CASE("a quoted dotted key and an escaped quote keep their lines")
{
  auto const error = failure_of(
      std::string{minimal}
      + "[board.\"I2C.1\"]\nmodel = \"m:C\"\nnote = \"a \\\" # b\"\n"
        "[clock]\ncpi = \"fast\"\n");
  CHECK(error.key() == "clock.cpi");
  CHECK(error.line() == 7);

  scratch_file const file{std::string{minimal} + "[board.\"I2C.1\"]\nmodel = \"m:C\"\nspeed = 4\n"};
  auto const         loaded = config::load_project(file.path());
  CHECK(loaded.file.board.contains("I2C.1"));
  CHECK(loaded.line_of("board.I2C.1.speed") == 5);
}

TEST_CASE("images without declaration lines have no load order")
{
  auto const error = failure_of(
      "image = { a = { elf = \"a.elf\" }, b = { elf = \"b.elf\" } }\n" + std::string{minimal});
  CHECK(error.key() == "image");
  CHECK(error.expected().starts_with("each image declared as"));
}

TEST_CASE("a board key without a model is an error")
{
  auto const error = failure_of(std::string{minimal} + "[board.UART4]\nconect = \"pty\"\n");
  CHECK(error.key() == "board.UART4.conect");
  CHECK(error.line() == 4);
  CHECK(
      error.expected()
      == "a key of board.UART4: connect, model, devices, or a model parameter with model set");
}

TEST_CASE("the clock refuses a cpi that is not finite and above 0, and a 0 Hz core")
{
  CHECK(failure_of(std::string{minimal} + "[clock]\ncpi = 0.0\n").key() == "clock.cpi");
  CHECK(failure_of(std::string{minimal} + "[clock]\ncpi = inf\n").key() == "clock.cpi");
  CHECK(failure_of(std::string{minimal} + "[clock]\ncpi = nan\n").key() == "clock.cpi");
  auto const zero = failure_of(std::string{minimal} + "[clock]\ncore_hz = 0\n");
  CHECK(zero.key() == "clock.core_hz");
  CHECK(zero.line() == 4);
}

TEST_CASE("an SPI device names its chip select, an I2C target its address, never both")
{
  scratch_file const file{
      std::string{minimal}
      + "[board.SPI1]\ndevices = [\n  { cs = \"PA4\", model = \"m:Flash\", size = 4 },\n"
        "  { model = \"m:Ss\" },\n]\n"};
  auto const  loaded  = config::load_project(file.path(), {});
  auto const& devices = loaded.file.board.at("SPI1").devices;
  REQUIRE(devices.size() == 2);
  CHECK(devices[0].cs == "PA4");
  CHECK_FALSE(devices[0].address.has_value());
  CHECK(devices[0].params.size() == 1);
  CHECK_FALSE(devices[1].cs.has_value());
  CHECK_FALSE(devices[1].address.has_value());

  auto const both = failure_of(
      std::string{minimal}
      + "[board.SPI1]\ndevices = [\n  { address = 0x50, cs = \"PA4\", model = \"m:C\" },\n]\n");
  CHECK(both.key() == "board.SPI1.devices[0].cs");
  CHECK(both.line() == 4);
  CHECK(both.expected() == "an address for an I2C target or a cs for an SPI device, not both");
}

TEST_CASE("a chip select is a pin name from P?0 to P?15")
{
  for(auto const pin : {"PA4", "PZ0", "PB15", "PC10"})
  {
    CAPTURE(pin);
    scratch_file const file{
        std::string{minimal}
        + std::format("[board.SPI1]\ndevices = [{{ cs = \"{}\", model = \"m:C\" }}]\n", pin)};
    CHECK(config::load_project(file.path(), {}).file.board.at("SPI1").devices[0].cs == pin);
  }
  for(auto const pin : {"PA16", "A4", "pa4", "Pa4", "PA", "PA04", "PA123", "P44", ""})
  {
    CAPTURE(pin);
    auto const error = failure_of(
        std::string{minimal}
        + std::format("[board.SPI1]\ndevices = [{{ cs = \"{}\", model = \"m:C\" }}]\n", pin));
    CHECK(error.key() == "board.SPI1.devices[0].cs");
    CHECK(error.expected() == std::format("a pin name such as PA4, pin 0 to 15, not {}", pin));
  }
}
