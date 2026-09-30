#pragma once

#include <popcorn/core/types.hpp>

#include <reflex/serde/toml_value.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::config
{
/** @brief Marks a member the project file must set. */
struct required_t
{};
/** @brief Annotation value of required_t. */
inline constexpr required_t required{};

/** @brief Marks a member load_project() computes, never read from the file. */
struct computed_t
{};
/** @brief Annotation value of computed_t. */
inline constexpr computed_t computed{};

/** @brief Marks the map member that collects the keys of a table no other member names. */
struct extra_keys_t
{};
/** @brief Annotation value of extra_keys_t. */
inline constexpr extra_keys_t extra_keys{};

/** @brief `[project]`. */
struct project_section
{
  /** @brief Project name, for reports. */
  std::string name{};
};

/** @brief `[mcu]`. */
struct mcu_section
{
  /** @brief Part number, such as `STM32U575QGIx`. */
  [[= required]] std::string part{};
  /** @brief Core of a multi-core part, empty for the only one. */
  std::optional<std::string> core{};
  /** @brief STM32CubeMX project, FR-CFG-3, resolved against the project file. */
  std::optional<std::string> ioc{};
  /** @brief SVD path, resolved against the project file, or `auto` for the local ST install. */
  std::string svd{"auto"};
  /** @brief MCU description JSON, resolved against the project file. Set, it replaces the
   * description built from `part` and `svd`, and its part must equal `part`. */
  std::optional<std::string> description{};
};

/** @brief `[image.<name>]`, exactly one of `elf`, `bin` and `hex`. */
struct image_entry
{
  /** @brief ELF file, resolved against the project file. */
  std::optional<std::string> elf{};
  /** @brief Raw binary, resolved against the project file, loaded at `at`. */
  std::optional<std::string> bin{};
  /** @brief Intel HEX file, resolved against the project file. */
  std::optional<std::string> hex{};
  /** @brief Load address of a raw binary. */
  std::optional<address> at{};
};

/** @brief One `[boot] memory_init` entry: bytes written at a symbol after the images load. */
struct memory_init_entry
{
  /** @brief Symbol whose address receives the bytes. */
  [[= required]] std::string symbol{};
  /** @brief Bytes, as hexadecimal digits, two per byte. */
  [[= required]] std::string bytes{};
};

/** @brief `[boot]`. */
struct boot_section
{
  /** @brief Vector table reset reads, empty for the description's default. */
  std::optional<address> vtor{};
  /** @brief Symbol boot runs to. */
  std::optional<std::string> until{};
  /** @brief Instructions boot may take. */
  std::optional<std::uint64_t> budget{};
  /** @brief Memory written after the images load. */
  std::vector<memory_init_entry> memory_init{};
  /** @brief First address of the call arena, `false` for none, empty for the default. */
  std::optional<reflex::serde::toml::value> arena_base{};
  /** @brief Size of the call arena in bytes, empty for the default. */
  std::optional<std::uint32_t> arena_size{};
};

/** @brief `[clock]`. */
struct clock_section
{
  /** @brief Core clock at power-on in Hz, empty for the description's reset clock. */
  std::optional<std::uint64_t> core_hz{};
  /** @brief Core cycles per instruction. */
  double cpi{1.0};
};

/**
 * @brief One device on a bus of `[board.<periph>] devices`: an I2C target by its address, or an
 *        SPI device by its chip select, never both.
 */
struct device_entry
{
  /** @brief 7-bit address of an I2C target, empty for an SPI device. */
  std::optional<popcorn::address> address{};
  /** @brief Chip select pin of an SPI device, such as `PA4`, active low. Empty for an I2C target,
   * or for an SPI device on the hardware SS output. */
  std::optional<std::string> cs{};
  /** @brief Model class, `module:Class`. */
  [[= required]] std::string model{};
  /** @brief Every other key, the model's constructor keyword arguments. */
  [[= extra_keys]] std::map<std::string, reflex::serde::toml::value> params{};
};

/** @brief `[board.<periph>]`. */
struct board_entry
{
  /** @brief Endpoint of a stream peripheral, such as `pty`. */
  std::optional<std::string> connect{};
  /** @brief Model class, `module:Class`, replacing the built-in model of the peripheral. */
  std::optional<std::string> model{};
  /** @brief Devices on the bus. */
  std::vector<device_entry> devices{};
  /** @brief Every other key, the model's constructor keyword arguments. */
  [[= extra_keys]] std::map<std::string, reflex::serde::toml::value> params{};
};

/** @brief `[models]`. */
struct models_section
{
  /** @brief Whether an unmodelled register access fails the run. */
  bool strict{};
  /** @brief Directories of user model modules, resolved against the project file. */
  std::vector<std::string> python{};
};

/** @brief A project file with the chosen `[configs.<name>]` applied, `configs` left out. */
struct project_file
{
  /** @brief `[project]`. */
  project_section project{};
  /** @brief `[mcu]`. */
  mcu_section mcu{};
  /** @brief `[image.<name>]`, loaded in declaration order. */
  std::map<std::string, image_entry> image{};
  /** @brief Image names in declaration order, FR-LOAD-5. */
  [[= computed]] std::vector<std::string> image_order{};
  /** @brief `[boot]`. */
  boot_section boot{};
  /** @brief `[clock]`. */
  clock_section clock{};
  /** @brief `[board.<periph>]`. */
  std::map<std::string, board_entry> board{};
  /** @brief `[models]`. */
  models_section models{};
};

/** @brief A loaded project file. */
struct loaded_project
{
  /** @brief The resolved content. */
  project_file file{};
  /** @brief Absolute path of the project file. */
  std::string path{};
  /** @brief Names of every `[configs.<name>]`, sorted. */
  std::vector<std::string> configs{};
  /** @brief The applied config, empty for none. */
  std::optional<std::string> config{};
  /** @brief SHA-256 of the canonical TOML of the resolved content, lower-case hex, what a
   * snapshot records as `config_digest`. */
  std::string digest{};
  /** @brief Line of each dotted key, a `[[table]]` entry as `table[<n>]`. */
  std::map<std::string, std::uint32_t> lines{};

  /**
   * @brief Line of a dotted key in the file, or of its nearest enclosing key.
   * @param key Dotted key, array entries as `[<n>]`.
   * @return The line, 1-based, 0 when unknown.
   */
  [[nodiscard]] auto line_of(std::string const& key) const -> std::uint32_t;
};

/**
 * @brief Reads a project file, applies a config, checks every key and binds the sections.
 * @param file Path of the project file.
 * @param config Name of the `[configs.<name>]` to apply, empty for none.
 * @return The project, paths resolved against the project file's directory.
 * @throws config_error with the file, the line when known, the dotted key and what was expected:
 *         unreadable file, TOML syntax, unknown key, wrong type, missing required key, unknown
 *         config, a config key that names no project key, an image with not exactly one file.
 */
auto load_project(std::filesystem::path const& file, std::optional<std::string_view> config = {})
    -> loaded_project;
}
