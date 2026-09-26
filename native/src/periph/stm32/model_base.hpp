#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief Register, resolved by name from the description, within its storage word. */
struct reg_ref
{
  /** @brief Byte offset of the register's word in the peripheral, a multiple of 4. */
  std::uint32_t offset{};
  /** @brief Position of the register's least significant bit in the word, 8 per byte lane. */
  std::uint8_t lsb{};
  /** @brief Width of the register in bits. */
  std::uint8_t width{};
};

/** @brief Bit field of a register, resolved by name from the description. */
struct field_ref
{
  /** @brief Byte offset of the register's word in the peripheral. */
  std::uint32_t offset{};
  /** @brief Position of the least significant bit. */
  std::uint8_t lsb{};
  /** @brief Width in bits. */
  std::uint8_t width{};

  /** @brief Bits of the field within the register word. */
  [[nodiscard]] constexpr auto mask() const noexcept -> std::uint32_t
  {
    return (width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << width) - 1u) << lsb;
  }

  /** @brief Value of the field in @p word, right-aligned. */
  [[nodiscard]] constexpr auto of(std::uint32_t word) const noexcept -> std::uint32_t
  {
    return (word & mask()) >> lsb;
  }

  /** @brief @p word with the field replaced by @p value, bits of @p value above the width
   * dropped. */
  [[nodiscard]] constexpr auto with(std::uint32_t word, std::uint32_t value) const noexcept
      -> std::uint32_t
  {
    return (word & ~mask()) | ((value << lsb) & mask());
  }
};

/**
 * @brief Value of a field in the storage, right-aligned.
 * @param regs Storage of the peripheral.
 * @param field Field to read.
 */
[[nodiscard]] inline auto get(register_file const& regs, field_ref field) -> std::uint32_t
{
  return field.of(regs.peek(field.offset));
}

/**
 * @brief Replaces a field in the storage, leaving the other bits of its word.
 * @param regs Storage of the peripheral.
 * @param field Field to write.
 * @param value New value, bits above the field width dropped.
 */
inline void update(register_file& regs, field_ref field, std::uint32_t value)
{
  regs.poke(field.offset, field.with(regs.peek(field.offset), value));
}

/**
 * @brief Model told when the RCC model changes a clock rate it publishes. Found by
 *        `dynamic_cast` over the machine's peers. Never called from a model's reset.
 */
class clock_listener
{
public:
  /** @brief Creates the listener. */
  clock_listener() = default;
  /** @brief Not copyable. */
  clock_listener(clock_listener const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(clock_listener const&) -> clock_listener& = delete;
  /** @brief Not movable. */
  clock_listener(clock_listener&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(clock_listener&&) -> clock_listener& = delete;

  /** @brief A published clock rate changed: re-derive timing from the clock domains. */
  virtual void on_clock_change() = 0;

protected:
  /** @brief Destroys the listener, only through its model. */
  ~clock_listener() = default;
};

/**
 * @brief Base of the built-in STM32 models: registers and fields resolved by name from the
 *        description, and the model state saved and loaded as BSON.
 * @tparam State Serializable state: every value the model needs to resume, its time anchors
 *         and its event generation included. load() restores it exactly and schedules nothing,
 *         since the machine restores the event queue before the models.
 */
template <typename State> class stm32_model : public model
{
public:
  void save(std::vector<std::byte>& out) const override
  {
    reflex::serde::bson::serializer{out}.dump(state_);
  }

  void load(std::span<std::byte const> in) override
  {
    state_ = reflex::serde::bson::deserializer{in}.template load<State>();
  }

protected:
  /**
   * @brief Keeps the context and the description.
   * @param ctx Context of the model, which outlives it.
   * @param desc Description of the peripheral, which outlives the model.
   * @param model_name Model name for configuration errors, such as `RCC`.
   */
  stm32_model(model_context& ctx, peripheral_desc const& desc, std::string_view model_name)
      : ctx_{ctx}, desc_{desc}, model_name_{model_name}
  {}

  /**
   * @brief Register of the peripheral by name, its word offset and its bits within the word.
   * @param name Register name as the description spells it, such as `RCC_CR`.
   * @throws config_error naming the part, the register and the model when the peripheral has
   *         no such register.
   */
  [[nodiscard]] auto reg(std::string_view name) const -> reg_ref
  {
    auto const& found = find_register(name);
    return {.offset = word_of(found), .lsb = lane_of(found), .width = found.width};
  }

  /**
   * @brief Field of a register of the peripheral by name, its bit position counted from the
   *        storage word, so a field of a register at offset 2 starts at bit 16 or above.
   * @param reg Register name as the description spells it.
   * @param field Field name as the description spells it.
   * @throws config_error naming the part, the register, the field and the model when the
   *         peripheral has no such register or field.
   */
  [[nodiscard]] auto field(std::string_view reg, std::string_view field) const -> field_ref
  {
    auto const& found  = find_register(reg);
    auto const  within = std::ranges::find(found.fields, field, &field_desc::name);
    if(within == found.fields.end())
    {
      missing(std::format("{}.{}", reg, field));
    }
    return {
        .offset = word_of(found),
        .lsb    = static_cast<std::uint8_t>(lane_of(found) + within->lsb),
        .width  = within->width};
  }

  /** @brief Context of the model. */
  model_context& ctx_;
  /** @brief Description of the peripheral. */
  peripheral_desc const& desc_;
  /** @brief State saved and loaded. */
  State state_{};

private:
  [[nodiscard]] static auto word_of(register_desc const& found) noexcept -> std::uint32_t
  {
    return found.offset & ~3u;
  }

  [[nodiscard]] static auto lane_of(register_desc const& found) noexcept -> std::uint8_t
  {
    return static_cast<std::uint8_t>(8 * (found.offset % 4));
  }

  [[nodiscard]] auto find_register(std::string_view name) const -> register_desc const&
  {
    auto const found = std::ranges::find(desc_.registers, name, &register_desc::name);
    if(found == desc_.registers.end())
    {
      missing(name);
    }
    return *found;
  }

  [[noreturn]] void missing(std::string_view path) const
  {
    throw config_error(
        ctx_.part.part, 0, std::format("peripherals.{}.registers.{}", desc_.name, path),
        std::format("a register the {} model needs", model_name_));
  }

  std::string model_name_;
};
}
