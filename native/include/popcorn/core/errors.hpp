#pragma once

#include <popcorn/core/types.hpp>

#include <reflex/exception.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace popcorn
{
/** @brief Base of every error popcorn raises; Unicorn errors never escape untranslated. */
class error : public reflex::runtime_error
{
public:
  using reflex::runtime_error::runtime_error;

  /** @brief Destroys the error. Declared out-of-line: the key function pins the vtable. */
  ~error() override;
};

/** @brief A machine description or configuration value is missing or malformed. */
class config_error : public error
{
public:
  /**
   * @brief Constructs a config_error.
   * @param file Source of the offending value.
   * @param line Line within @p file, 0 when unknown.
   * @param key Key or path of the offending value.
   * @param expected What was expected at @p key.
   */
  config_error(std::string file, std::uint32_t line, std::string key, std::string expected);

  /** @brief Source of the offending value. */
  [[nodiscard]] auto file() const noexcept -> std::string const&;
  /** @brief Line within file(), 0 when unknown. */
  [[nodiscard]] auto line() const noexcept -> std::uint32_t;
  /** @brief Key or path of the offending value. */
  [[nodiscard]] auto key() const noexcept -> std::string const&;
  /** @brief What was expected at key(). */
  [[nodiscard]] auto expected() const noexcept -> std::string const&;

private:
  std::string   file_;
  std::uint32_t line_;
  std::string   key_;
  std::string   expected_;
};

/** @brief An ELF image cannot be loaded as described. */
class image_error : public error
{
public:
  /**
   * @brief Constructs an image_error.
   * @param segment Name or index of the offending segment.
   * @param addr Address involved, 0 when not applicable.
   * @param region Description of the expected or conflicting region.
   */
  image_error(std::string segment, address addr, std::string region);

  /** @brief Name or index of the offending segment. */
  [[nodiscard]] auto segment() const noexcept -> std::string const&;
  /** @brief Address involved, 0 when not applicable. */
  [[nodiscard]] auto addr() const noexcept -> address;
  /** @brief Description of the expected or conflicting region. */
  [[nodiscard]] auto region() const noexcept -> std::string const&;

private:
  std::string segment_;
  address     addr_;
  std::string region_;
};

/** @brief A symbol lookup found no exact match. */
class symbol_error : public error
{
public:
  /**
   * @brief Constructs a symbol_error.
   * @param name Symbol that was looked up.
   * @param close_matches Candidate names close to @p name, closest first.
   */
  symbol_error(std::string name, std::vector<std::string> close_matches);

  /** @brief Symbol that was looked up. */
  [[nodiscard]] auto name() const noexcept -> std::string const&;
  /** @brief Candidate names close to name(), closest first. */
  [[nodiscard]] auto close_matches() const noexcept -> std::vector<std::string> const&;

private:
  std::string              name_;
  std::vector<std::string> close_matches_;
};

/** @brief The emulated engine failed outside a fault or a budget, such as during setup. */
class emulation_error : public error
{
public:
  using error::error;
};

/** @brief A run stopped on a processor fault. */
class fault_error : public emulation_error
{
public:
  /**
   * @brief Constructs a fault_error.
   * @param where Location at which the fault was taken.
   * @param fault Decoded fault.
   */
  fault_error(location where, fault_info fault);

  /** @brief Location at which the fault was taken. */
  [[nodiscard]] auto where() const noexcept -> owned_location const&;
  /** @brief Decoded fault. */
  [[nodiscard]] auto fault() const noexcept -> fault_info const&;

private:
  owned_location where_;
  fault_info     fault_;
};

/** @brief A run exceeded its instruction budget. */
class budget_exceeded : public emulation_error
{
public:
  /**
   * @brief Constructs a budget_exceeded.
   * @param where Location at which the budget ran out.
   * @param budget Budget that was exceeded.
   */
  budget_exceeded(location where, icount budget);

  /** @brief Location at which the budget ran out. */
  [[nodiscard]] auto where() const noexcept -> owned_location const&;
  /** @brief Budget that was exceeded. */
  [[nodiscard]] auto budget() const noexcept -> icount;

private:
  owned_location where_;
  icount         budget_;
};

/** @brief A peripheral model rejected or could not service an access. */
class model_error : public error
{
public:
  /**
   * @brief Constructs a model_error.
   * @param peripheral Name of the peripheral.
   * @param reg Name of the register, empty when not register-specific.
   * @param offset Offset within the peripheral.
   */
  model_error(std::string peripheral, std::string reg, address offset);

  /** @brief Name of the peripheral. */
  [[nodiscard]] auto peripheral() const noexcept -> std::string const&;
  /** @brief Name of the register, empty when not register-specific. */
  [[nodiscard]] auto reg() const noexcept -> std::string const&;
  /** @brief Offset within the peripheral. */
  [[nodiscard]] auto offset() const noexcept -> address;

private:
  std::string peripheral_;
  std::string reg_;
  address     offset_;
};

/** @brief The requested operation needs the machine stopped, but it is running. */
class machine_running : public error
{
public:
  using error::error;
};

/** @brief The requested operation cannot run while another one is in progress. */
class machine_busy : public error
{
public:
  using error::error;
};

/** @brief The operation is not implemented yet. */
class not_implemented : public error
{
public:
  using error::error;
};
}
