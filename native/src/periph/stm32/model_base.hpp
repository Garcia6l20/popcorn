#pragma once

#include <popcorn/core/clock.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/core_hold.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include "debug_freeze.hpp"
#include "kernel_gate.hpp"
#include "rcc_resets.hpp"

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
   * @param name Register name or alias as the description spells it, such as `RCC_CR`.
   * @throws config_error naming the part, the register and the model when the peripheral has
   *         no such register.
   */
  [[nodiscard]] auto reg(std::string_view name) const -> reg_ref
  {
    auto const& found = describe(name);
    return {.offset = word_of(found), .lsb = lane_of(found), .width = found.width};
  }

  /**
   * @brief Field of a register of the peripheral by name, its bit position counted from the
   *        storage word, so a field of a register at offset 2 starts at bit 16 or above.
   * @param reg Register name or alias as the description spells it.
   * @param field Field name as the description spells it.
   * @throws config_error naming the part, the register, the field and the model when the
   *         peripheral has no such register or field.
   */
  [[nodiscard]] auto field(std::string_view reg, std::string_view field) const -> field_ref
  {
    auto const& found  = describe(reg);
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

  /**
   * @brief Whether the peripheral has a register of that name.
   * @param name Register name or alias as the description spells it.
   */
  [[nodiscard]] auto has_register(std::string_view name) const -> bool
  {
    return find_register(desc_, name) != nullptr;
  }

  /**
   * @brief Description of a register of the peripheral by name.
   * @param name Register name or alias as the description spells it.
   * @throws config_error as reg() does.
   */
  [[nodiscard]] auto describe(std::string_view name) const -> register_desc const&
  {
    auto const* found = find_register(desc_, name);
    if(found == nullptr)
    {
      missing(name);
    }
    return *found;
  }

  /**
   * @brief Logs a model warning through the machine services. The model records no pc: the
   *        services fill in the current one.
   * @param reg Register the warning is about, as the description spells it.
   * @param message Human-readable text.
   */
  void warn(std::string_view reg, std::string message) const
  {
    ctx_.services.log(
        {.kind       = core::log_kind::model_warning,
         .peripheral = desc_.name,
         .reg        = std::string{reg},
         .message    = std::move(message)});
  }

  /**
   * @brief Whether DBGMCU freezes this peripheral while the core is held: its
   *        `DBG_<instance>_STOP` field, read through the debug_freeze peer.
   * @param logged The model's flag of the warnings below, set when one is logged.
   * @param unit What the field names, the instance by default, such as `GPDMA3` for a channel.
   * @return Whether the field is set. False when the machine has no DBGMCU model or the part no
   *         such field, the peripheral then runs on unfrozen. Without a DBGMCU model the first
   *         core_hold_listener of the machine, in description order, logs one warning for the
   *         machine. With one but no field, each model logs its own warning once.
   * @throws model_error under strict_peripherals when the field cannot be read. Every
   *         core_hold_listener calls it at each hold, before any other test and before changing
   *         state, so a strict machine refuses the hold whatever the listener's own state.
   */
  [[nodiscard]] auto debug_frozen(bool& logged, std::string_view unit = {}) const -> bool
  {
    auto const name =
        std::format("DBG_{}_STOP", unit.empty() ? std::string_view{desc_.name} : unit);
    model const* listener = nullptr;
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto const* const freeze = dynamic_cast<debug_freeze const*>(peer))
      {
        if(auto const set = freeze->frozen(name))
        {
          return *set;
        }
        if(not std::exchange(logged, true))
        {
          warn(
              "", std::format(
                      "the core is held and DBGMCU has no {} field to read: {} runs on, unfrozen",
                      name, desc_.name));
        }
        if(ctx_.strict_peripherals)
        {
          throw model_error(desc_.name, name, 0);
        }
        return false;
      }
      if(listener == nullptr and dynamic_cast<core_hold_listener const*>(peer) != nullptr)
      {
        listener = peer;
      }
    }
    if(listener == this and not std::exchange(logged, true))
    {
      warn(
          "", "the core is held and the machine has no DBGMCU model: debug freezes are not "
              "modelled, every peripheral runs on");
    }
    if(ctx_.strict_peripherals)
    {
      throw model_error("DBGMCU", "", 0);
    }
    return false;
  }

  /**
   * @brief Schedules an event of the model a virtual time from now, kept at that virtual time
   *        across core clock changes.
   * @param delay Virtual time until the event.
   * @param kind Event kind, passed back to on_event().
   * @param payload Payload passed back to on_event(), usually a generation.
   */
  void schedule_in(std::chrono::nanoseconds delay, std::uint32_t kind, std::uint64_t payload)
  {
    auto const ps = static_cast<std::uint64_t>(delay.count()) * 1000u;
    ctx_.events.schedule_at(vtime{ctx_.clock.time().ps + ps}, kind, payload, ctx_.clock);
  }

  /**
   * @brief IRQ line of the peripheral by the description's IRQ name.
   * @param name IRQ name as the description spells it, such as `ICACHE_IRQn`.
   * @throws config_error naming the part, the IRQ and the model when the part has no such IRQ or
   *         the peripheral does not raise it.
   */
  [[nodiscard]] auto irq(std::string_view name) const -> irq_line&
  {
    auto const named = std::ranges::find(ctx_.part.irqs, name, &irq_desc::name);
    if(named != ctx_.part.irqs.end())
    {
      auto const sources = irq_sources(ctx_.part, desc_, ctx_.trustzone);
      auto const at      = std::ranges::find(sources, named->number);
      if(at != sources.end())
      {
        return ctx_.irqs[static_cast<std::size_t>(at - sources.begin())];
      }
    }
    throw config_error(
        ctx_.part.part, 0, std::format("peripherals.{}.irqs.{}", desc_.name, name),
        std::format("an IRQ the {} model raises", model_name_));
  }

  /**
   * @brief Throws the configuration error of a register or field the model needs.
   * @param path Register, or register and field joined by a dot.
   */
  [[noreturn]] void missing(std::string_view path) const
  {
    throw config_error(
        ctx_.part.part, 0, std::format("peripherals.{}.registers.{}", desc_.name, path),
        std::format("a register the {} model needs", model_name_));
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

  std::string model_name_;
};

/**
 * @brief Word a guest write leaves in storage before register semantics apply.
 * @param old Stored word.
 * @param offset Byte offset of the access in the peripheral.
 * @param size Access size in bytes, 1, 2 or 4.
 * @param value Written value, right-aligned to @p size.
 */
[[nodiscard]] constexpr auto merge_write(
    std::uint32_t old,
    std::uint32_t offset,
    std::uint8_t  size,
    std::uint32_t value) noexcept -> std::uint32_t
{
  auto const shift = 8u * (offset % 4u);
  auto const lanes = size >= 4 ? ~std::uint32_t{} : ((std::uint32_t{1} << (8u * size)) - 1u);
  return (old & ~(lanes << shift)) | ((value & lanes) << shift);
}

/**
 * @brief Bytes of a stored word a guest read returns, right-aligned.
 * @param word Stored word.
 * @param offset Byte offset of the access in the peripheral.
 * @param size Access size in bytes, 1, 2 or 4.
 */
[[nodiscard]] constexpr auto
    read_lanes(std::uint32_t word, std::uint32_t offset, std::uint8_t size) noexcept
    -> std::uint32_t
{
  auto const lanes = size >= 4 ? ~std::uint32_t{} : ((std::uint32_t{1} << (8u * size)) - 1u);
  return (word >> (8u * (offset % 4u))) & lanes;
}

/**
 * @brief Bits of one register word a transaction cannot reach: a read returns 0 in the RAZ
 *        bits, a write leaves the WI bits as they were, RM0456 3.5.5 and 3.6.3.
 */
struct bus_guard
{
  /** @brief Bits a read returns as 0. */
  std::uint32_t raz{};
  /** @brief Bits a write does not change. */
  std::uint32_t wi{};

  /** @brief Both RAZ and WI. */
  [[nodiscard]] static constexpr auto both(std::uint32_t bits) noexcept -> bus_guard
  {
    return {.raz = bits, .wi = bits};
  }

  /** @brief The union of two guards. */
  [[nodiscard]] friend constexpr auto operator|(bus_guard a, bus_guard b) noexcept -> bus_guard
  {
    return {.raz = a.raz | b.raz, .wi = a.wi | b.wi};
  }

  /** @brief Member-wise equality. */
  friend auto operator==(bus_guard const&, bus_guard const&) -> bool = default;
};

/**
 * @brief Guard of bits whose security and privilege follow one rule.
 * @param bits Bits of the register word.
 * @param secure Whether the bits are Secure: a Non-secure transaction cannot reach them.
 * @param privileged Whether the bits are privileged: an unprivileged transaction cannot reach
 *        them.
 * @param ctx Attributes of the transaction.
 */
[[nodiscard]] constexpr auto
    guard_of(std::uint32_t bits, bool secure, bool privileged, access_ctx const& ctx) noexcept
    -> bus_guard
{
  return bus_guard::both(
      (secure and not ctx.secure) or (privileged and not ctx.privileged) ? bits : 0);
}

/** @brief Cross rule of the description from one register, resolved: a write of 1 to the source
 * bits clears or sets the target field. */
struct cross_ref
{
  /** @brief Bits of the source field within the source register's word. */
  std::uint32_t source{};
  /** @brief Field acted on. */
  field_ref target;
  /** @brief What the write does to the target. */
  cross_effect effect{};
};

/**
 * @brief Register a model serves through a write handler.
 * @tparam Derived Model class.
 */
template <typename Derived> struct write_entry
{
  /** @brief Register name as the description spells it. */
  std::string_view reg;
  /** @brief Called after a guest write stored the register's writable bits, with the word
   * before and after. Null for a register served as storage. */
  void (Derived::*on_write)(std::uint32_t old, std::uint32_t now);
};

/**
 * @brief A model class that guards some of its register bits per transaction: `guard(word,
 *        ctx, write)` gives the bits of the word at byte offset `word` that the access cannot
 *        reach, and may raise an illegal access event.
 */
template <typename Derived>
concept guarded_model = requires(Derived& d, std::uint32_t word, access_ctx const& ctx) {
  { d.guard(word, ctx, true) } -> std::same_as<bus_guard>;
};

/**
 * @brief Model serving its claimed registers through one write handler each. Reads return the
 *        stored word. A write merges the access into the stored word, keeps the bits the
 *        description marks read-only, stores the result, then calls the register's handler.
 *        A guarded_model Derived gives per access the bits the transaction cannot reach: a read
 *        returns them as 0, a write keeps their stored value, or the value that writes nothing
 *        for a field whose write is an action, such as 0 for a write-1-to-clear flag, so the
 *        handler sees no write to them.
 * @tparam Derived Model class.
 * @tparam State Serializable state, as for stm32_model.
 */
template <typename Derived, typename State> class dispatch_model : public stm32_model<State>
{
public:
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    std::vector<std::string> names;
    for(auto const& entry : bound_)
    {
      names.emplace_back(entry.name);
    }
    return names;
  }

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override
  {
    auto const word  = offset & ~3u;
    auto const found = std::ranges::find(bound_, word, &bound::offset);
    auto       mask  = found != bound_.end() ? found->readable : ~std::uint32_t{};
    if constexpr(guarded_model<Derived>)
    {
      mask &= ~static_cast<Derived&>(*this).guard(word, ctx, false).raz;
    }
    return read_lanes(regs.peek(word) & mask, offset, size);
  }

  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t,
      std::uint32_t value,
      access_ctx    ctx) -> std::expected<void, bus_error> override
  {
    auto const word  = offset & ~3u;
    auto const found = std::ranges::find(bound_, word, &bound::offset);
    if constexpr(std::derived_from<Derived, bus_reset_listener>)
    {
      if(held_in_reset())
      {
        if constexpr(guarded_model<Derived>)
        {
          if(found != bound_.end())
          {
            static_cast<void>(static_cast<Derived&>(*this).guard(word, ctx, true));
          }
        }
        return {};
      }
    }
    if(found != bound_.end())
    {
      auto const old      = regs.peek(word);
      auto const merged   = merge_write(old, offset, size, value);
      auto       writable = found->writable;
      auto       kept     = old;
      if constexpr(guarded_model<Derived>)
      {
        auto const wi = static_cast<Derived&>(*this).guard(word, ctx, true).wi & writable;
        writable &= ~wi;
        kept = (old & ~(wi & found->actions)) | (wi & found->actions & found->idle);
      }
      auto const now = (merged & writable) | (kept & ~writable);
      regs.poke(word, now);
      if(found->handler != nullptr)
      {
        (static_cast<Derived&>(*this).*found->handler)(old, now);
      }
    }
    return {};
  }

protected:
  /**
   * @brief Resolves every entry's register by name.
   * @param ctx Context of the model.
   * @param desc Description of the peripheral.
   * @param model_name Model name for configuration errors.
   * @param table Registers and their write handlers.
   * @param trustzone_table Registers claimed in addition under TrustZone only.
   * @throws config_error when the description lacks a register of @p table, or under
   *         TrustZone of @p trustzone_table.
   */
  dispatch_model(
      model_context&                        ctx,
      peripheral_desc const&                desc,
      std::string_view                      model_name,
      std::span<write_entry<Derived> const> table,
      std::span<write_entry<Derived> const> trustzone_table = {})
      : stm32_model<State>{ctx, desc, model_name}
  {
    bind(table);
    if(ctx.trustzone)
    {
      bind(trustzone_table);
    }
    if constexpr(std::derived_from<Derived, bus_reset_listener>)
    {
      auto const rcc =
          std::ranges::find(ctx.part.peripherals, std::string_view{"RCC"}, &peripheral_desc::name);
      if(rcc != ctx.part.peripherals.end() and not reset_bit_of(*rcc, desc.name))
      {
        throw config_error(
            ctx.part.part, 0,
            std::format(
                "peripherals.RCC.registers.RCC_xxxRSTR.fields.{}", reset_bit_name(desc.name)),
            std::format(
                "the RCC reset bit of {}, which the {} model resets", desc.name, model_name));
      }
    }
  }

  /**
   * @brief Whether RCC holds the peripheral in reset, its RCC_xxxRSTR bit 1.
   * @return The bit, false without an RCC model.
   */
  [[nodiscard]] auto held_in_reset() -> bool
  {
    if(not reset_resolved_ and not this->ctx_.peers.all().empty())
    {
      reset_resolved_ = true;
      for(auto* const peer : this->ctx_.peers.all())
      {
        if(auto const* const gate = dynamic_cast<kernel_gate const*>(peer))
        {
          reset_gate_ = gate;
          reset_line_ = gate->reset_line(this->desc_.name);
          break;
        }
      }
    }
    return reset_gate_ != nullptr and reset_line_ and reset_gate_->held_in_reset(*reset_line_);
  }

  /**
   * @brief The description's cross rules from a register, resolved. A claimed register loses
   *        the register layer's cross rules, so its write handler applies them.
   * @param reg Source register name or alias as the description spells it.
   * @throws config_error when a rule names a register or field the peripheral lacks.
   */
  [[nodiscard]] auto crosses_from(std::string_view reg) const -> std::vector<cross_ref>
  {
    auto const*            source = &this->describe(reg);
    std::vector<cross_ref> out;
    for(auto const& rule : this->desc_.cross_rules)
    {
      if(find_register(this->desc_, rule.reg) == source)
      {
        out.push_back(
            {.source = this->field(rule.reg, rule.field).mask(),
             .target = this->field(rule.target_reg, rule.target_field),
             .effect = rule.effect});
      }
    }
    return out;
  }

  /**
   * @brief Applies resolved cross rules to the storage for a written word.
   * @param crosses Rules from crosses_from().
   * @param written Word the write left, after the guard.
   */
  void apply_crosses(std::span<cross_ref const> crosses, std::uint32_t written)
  {
    for(auto const& cross : crosses)
    {
      if((written & cross.source) != 0)
      {
        update(
            this->ctx_.regs, cross.target,
            cross.effect == cross_effect::set ? ~std::uint32_t{} : std::uint32_t{});
      }
    }
  }

  /**
   * @brief Claims the registers of @p table and resolves them by name.
   * @param table Registers and their write handlers.
   * @throws config_error when the description lacks a register of @p table, or when an entry
   *         with no handler claims the source register of a description cross rule or a
   *         register with a field whose write effect is not `modify`, which the claim would
   *         silently drop: a claimed word stores the written bits as they are.
   */
  void bind(std::span<write_entry<Derived> const> table)
  {
    for(auto const& entry : table)
    {
      auto const& described = this->describe(entry.reg);
      if(entry.on_write == nullptr)
      {
        for(auto const& rule : this->desc_.cross_rules)
        {
          if(find_register(this->desc_, rule.reg) == &described)
          {
            throw config_error(
                this->ctx_.part.part, 0,
                std::format(
                    "peripherals.{}.cross_rules.{}.{}", this->desc_.name, rule.reg, rule.field),
                std::format(
                    "a write handler for {}, a cross rule source the model claims",
                    described.name));
          }
        }
        for(auto const& field : described.fields)
        {
          if(field.on_write != write_effect::modify)
          {
            throw config_error(
                this->ctx_.part.part, 0,
                std::format(
                    "peripherals.{}.registers.{}.fields.{}.on_write", this->desc_.name,
                    described.name, field.name),
                std::format(
                    "a write handler for {}, whose field {} has the {} write effect",
                    described.name, field.name, field.on_write));
          }
        }
      }
      auto const    at       = this->reg(entry.reg);
      std::uint32_t writable = 0;
      std::uint32_t readable = 0;
      std::uint32_t actions  = 0;
      std::uint32_t idle     = 0;
      auto const    whole = field_ref{.offset = at.offset, .lsb = at.lsb, .width = at.width}.mask();
      if(described.fields.empty())
      {
        if(described.acc == access::read_only)
        {
          this->missing(std::format("{}.fields", entry.reg));
        }
        writable = whole;
        readable =
            described.acc == access::write_only or described.acc == access::write_once ? 0 : whole;
      }
      for(auto const& field : described.fields)
      {
        auto const bits =
            field_ref{
                .offset = at.offset,
                .lsb    = static_cast<std::uint8_t>(at.lsb + field.lsb),
                .width  = field.width}
                .mask();
        if(field.acc != access::read_only)
        {
          writable |= bits;
        }
        if(field.acc != access::write_only and field.acc != access::write_once)
        {
          readable |= bits;
        }
        if(field.acc == access::write_only or field.on_write != write_effect::modify)
        {
          actions |= bits;
        }
        if(field.on_write == write_effect::zero_to_clear
           or field.on_write == write_effect::zero_to_set
           or field.on_write == write_effect::zero_to_toggle)
        {
          idle |= bits;
        }
      }
      bound_.push_back(
          {.name     = described.name,
           .offset   = at.offset,
           .writable = writable,
           .readable = readable,
           .actions  = actions,
           .idle     = idle,
           .handler  = entry.on_write});
    }
  }

private:
  struct bound
  {
    std::string_view name;
    std::uint32_t    offset{};
    std::uint32_t    writable{};
    std::uint32_t    readable{};
    std::uint32_t    actions{};
    std::uint32_t    idle{};
    void (Derived::*handler)(std::uint32_t old, std::uint32_t now);
  };

  std::vector<bound>         bound_;
  kernel_gate const*         reset_gate_ = nullptr;
  std::optional<std::size_t> reset_line_;
  bool                       reset_resolved_ = false;
};
}
