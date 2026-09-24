#pragma once

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <meta>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::core
{
/** @brief Annotation of a trace_kind enumerator: the u32 payload words its `.pctr` record
 * carries after the header. */
struct pctr_payload
{
  /** @brief Payload words. */
  std::uint8_t words{};
};

/**
 * @brief Kind of a trace record, the `kind` byte of a `.pctr` trace file. Each enumerator value
 *        is the byte written to the file, pinned by an explicit value and a check that it equals
 *        the enumerator's position, so a reorder fails to compile. The pctr_payload annotation
 *        gives the payload size.
 *
 * - block: payload[0] pc, aux the block's instruction count.
 * - mem: payload pc, address, value. flags the access size, trace_write_flag for a write.
 * - mmio: as mem, aux the flat register id of `flat_register_id()`.
 * - irq: no payload, flags 1 on entry and 0 on exit, aux the exception number.
 * - call: payload pc and r0, aux the index of the called symbol in the machine's symbol table,
 *   the symbols of its loaded images concatenated in load order.
 * - str: file only, aux bytes of text follow the header, padded to 4, dt is the string id. Its
 *   annotation counts no fixed payload.
 * - time: payload[0] and payload[1] the absolute instruction count, low word first, written when
 *   a delta does not fit dt or time moved backwards. The next record counts from it.
 * - stop: payload[0] pc, aux the `stop_reason` value.
 *
 * A register or symbol id of 0xffff or more is written as trace_id_overflow, see trace_id(), and
 * a reader resolves the record by its payload address instead.
 */
enum class[[= reflex::derive{reflex::Format}]] trace_kind : std::uint8_t
{
  block[[= pctr_payload{1}]] = 0,
  mem[[= pctr_payload{3}]]   = 1,
  mmio[[= pctr_payload{3}]]  = 2,
  irq[[= pctr_payload{0}]]   = 3,
  call[[= pctr_payload{2}]]  = 4,
  str[[= pctr_payload{0}]]   = 5,
  time[[= pctr_payload{2}]]  = 6,
  stop[[= pctr_payload{1}]]  = 7,
};

static_assert(
    [] {
      std::size_t position = 0;
      for(std::meta::info const e : std::meta::enumerators_of(^^trace_kind))
      {
        if(std::to_underlying(std::meta::extract<trace_kind>(e)) != position++)
        {
          return false;
        }
      }
      return true;
    }(),
    "trace_kind values are the .pctr kind bytes, each equal to its position");

/** @brief aux value of an mmio or call record whose id is 0xffff or more. Reserved: no id is
 * written as this value. */
inline constexpr std::uint16_t trace_id_overflow = 0xffff;

/**
 * @brief aux value of a register or symbol id.
 * @param id Flat register id or symbol index.
 * @return @p id when below 0xffff, trace_id_overflow otherwise.
 */
constexpr auto trace_id(std::uint32_t id) noexcept -> std::uint16_t
{
  return id < trace_id_overflow ? static_cast<std::uint16_t>(id) : trace_id_overflow;
}

/**
 * @brief Fixed payload words of a record kind in a `.pctr` file, read from its pctr_payload
 *        annotation by reflection.
 * @param kind Record kind.
 */
constexpr auto pctr_payload_words(trace_kind kind) noexcept -> std::size_t
{
  template for(constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^trace_kind)))
  {
    if(kind == [:e:])
    {
      constexpr auto annotations =
          std::define_static_array(std::meta::annotations_of_with_type(e, ^^pctr_payload));
      static_assert(annotations.size() == 1, "every trace_kind carries one pctr_payload");
      return std::meta::extract<pctr_payload>(annotations[0]).words;
    }
  }
  return 0;
}

/** @brief `flags` bit of a mem or mmio record set for a write. */
inline constexpr std::uint8_t trace_write_flag = 0x80;

/** @brief Size in bytes of the `.pctr` record header `<u8 kind, u8 flags, u16 aux, u32 dt>`. */
inline constexpr std::size_t pctr_header_size = 8;

/** @brief Largest fixed `.pctr` payload in bytes, three u32 words of a mem or mmio record. */
inline constexpr std::size_t pctr_max_payload_size = 12;

/**
 * @brief One trace record, laid out as the largest fixed `.pctr` record: the header
 *        `<u8 kind, u8 flags, u16 aux, u32 dt>` then three payload words, little endian on the
 * file. Records with a shorter payload leave the unused words 0, the file writer drops them. Plain
 * data, trivially copyable.
 */
struct trace_record
{
  /** @brief Record kind. */
  trace_kind kind{};
  /** @brief Kind-specific flags, such as the access size and write bit. */
  std::uint8_t flags{};
  /** @brief Kind-specific auxiliary value, such as a block instruction count or string id. */
  std::uint16_t aux{};
  /** @brief Instructions executed since the previous record of the same stream, stamped by
   * trace_ring::append() in the ring and by the machine for a trace sink. */
  std::uint32_t dt{};
  /** @brief Kind-specific payload words. */
  std::array<std::uint32_t, 3> payload{};

  /** @brief Member-wise equality. */
  friend auto operator==(trace_record const&, trace_record const&) -> bool = default;
};

static_assert(std::is_trivially_copyable_v<trace_record>);
static_assert(std::is_standard_layout_v<trace_record>);
static_assert(sizeof(trace_record) == pctr_header_size + pctr_max_payload_size);
static_assert(
    std::ranges::max(
        std::define_static_array(
            std::meta::enumerators_of(^^trace_kind) | std::views::transform([](std::meta::info e) {
              return static_cast<std::size_t>(
                  std::meta::extract<pctr_payload>(
                      std::meta::annotations_of_with_type(e, ^^pctr_payload).front())
                      .words);
            })))
    * sizeof(std::uint32_t)
    == pctr_max_payload_size);
static_assert(alignof(trace_record) == 4);
static_assert(offsetof(trace_record, dt) == 4);
static_assert(offsetof(trace_record, payload) == pctr_header_size);

/**
 * @brief Set of trace kinds, one bit per trace_kind, for selecting what a trace sink receives.
 */
class trace_kinds
{
public:
  /** @brief The empty set. */
  constexpr trace_kinds() noexcept = default;

  /**
   * @brief The set of the given kinds.
   * @param kinds Kinds in the set.
   */
  constexpr trace_kinds(std::initializer_list<trace_kind> kinds) noexcept
  {
    for(trace_kind const kind : kinds)
    {
      bits_ |= bit(kind);
    }
  }

  /** @brief Every trace kind, enumerated by reflection. */
  [[nodiscard]] static constexpr auto all() noexcept -> trace_kinds
  {
    trace_kinds every;
    template for(constexpr auto e :
                 std::define_static_array(std::meta::enumerators_of(^^trace_kind)))
    {
      every.bits_ |= bit([:e:]);
    }
    return every;
  }

  /**
   * @brief Whether a kind is in the set.
   * @param kind Kind to look up.
   */
  [[nodiscard]] constexpr auto contains(trace_kind kind) const noexcept -> bool
  {
    return (bits_ & bit(kind)) != 0;
  }

  /** @brief Member-wise equality. */
  friend constexpr auto operator==(trace_kinds, trace_kinds) -> bool = default;

private:
  static constexpr auto bit(trace_kind kind) noexcept -> std::uint32_t
  {
    return std::uint32_t{1} << std::to_underlying(kind);
  }

  std::uint32_t bits_{};
};

/**
 * @brief Receives trace records in batches, oldest first. A sink has its own dt stream: each
 *        record's dt counts from the previous record delivered to that sink, the first from the
 *        machine's instruction count when the sink was added, with time records as in the ring.
 *        Runs on the thread that runs the slice.
 */
using trace_sink = std::function<void(std::span<trace_record const> records)>;

/**
 * @brief Fixed-size ring of the most recent trace records, always on. The
 *        oldest record is overwritten when the ring is full. Records are delta-stamped: each
 *        record's dt counts the instructions since the record before it, and origin() gives the
 *        absolute count the oldest kept record counts from. Owned by the machine, written through
 *        `machine_services::trace`, read by stop reports and ring dumps. Neither copyable nor
 *        movable.
 */
class trace_ring
{
public:
  /**
   * @brief Creates an empty ring.
   * @param capacity Number of records kept.
   */
  explicit trace_ring(std::size_t capacity) pre(capacity > 0);

  /** @brief Not copyable. */
  trace_ring(trace_ring const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(trace_ring const&) -> trace_ring& = delete;
  /** @brief Not movable. */
  trace_ring(trace_ring&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(trace_ring&&) -> trace_ring& = delete;
  /** @brief Destroys the ring. */
  ~trace_ring();

  /**
   * @brief Appends a record, stamping its dt with the instructions since the previous append.
   *        A delta past the u32 range, or a @p now before the previous append, first appends a
   *        time record carrying @p now.
   * @param record Record to append, its dt ignored.
   * @param now Instruction count at which the record happened.
   */
  void append(trace_record record, icount now);

  /**
   * @brief Moves the time base when the machine's instruction count jumps, on a restore or a
   *        reset: appends a time record carrying @p now, and the next append counts from it. Kept
   *        records stay.
   * @param now New instruction count.
   */
  void rebase(icount now);

  /** @brief Number of records kept, at most capacity(). */
  [[nodiscard]] auto size() const noexcept -> std::size_t;

  /** @brief Number of records the ring keeps before it overwrites the oldest. */
  [[nodiscard]] auto capacity() const noexcept -> std::size_t;

  /** @brief Records appended since construction or clear(), overwritten ones included. */
  [[nodiscard]] auto appended() const noexcept -> std::uint64_t;

  /** @brief Absolute instruction count the dt of the oldest kept record counts from. */
  [[nodiscard]] auto origin() const noexcept -> icount;

  /** @brief Kept records, oldest first. */
  [[nodiscard]] auto records() const -> std::vector<trace_record>;

  /**
   * @brief Drops every record.
   * @param now Instruction count the next append counts its dt from.
   */
  void clear(icount now) noexcept;

private:
  void push(trace_record const& stamped);
  void push_time(icount now);

  std::vector<trace_record> slots_;
  std::size_t               next_     = 0;
  std::size_t               size_     = 0;
  std::uint64_t             appended_ = 0;
  icount                    origin_{};
  icount                    last_{};
};
}
