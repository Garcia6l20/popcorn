#pragma once

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/core/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace popcorn::obs
{
/** @brief A named address range, a register or a symbol. */
struct named_range
{
  /** @brief First address. */
  address start{};
  /** @brief Size in bytes. */
  std::uint32_t size{};
  /** @brief Name. */
  std::string name{};

  /** @brief Member-wise equality. */
  friend auto operator==(named_range const&, named_range const&) -> bool = default;
};

/** @brief Names a `.pctr` writer gives the ids of mmio and call records. */
struct pctr_names
{
  /** @brief Register names by flat register id, `<peripheral>.<register>`. */
  std::vector<std::string> registers{};
  /** @brief Register address ranges, for mmio records whose id overflowed. */
  std::vector<named_range> register_ranges{};
  /** @brief Symbol names by index in the machine's symbol table. */
  std::vector<std::string> symbols{};
  /** @brief Symbol address ranges, for call records whose index overflowed. */
  std::vector<named_range> symbol_ranges{};

  /** @brief Member-wise equality. */
  friend auto operator==(pctr_names const&, pctr_names const&) -> bool = default;
};

/**
 * @brief Writes trace records to a `.pctr` version 1 file: `PCTR`, u16 version 1, u16 flags 0,
 *        a TIME record carrying the origin, then the records, each its header
 *        `<u8 kind, u8 flags, u16 aux, u32 dt>` and its pctr_payload_words() payload words, little
 *        endian. An mmio or call record's id becomes a string id, its name written first as a STR
 *        record `{str, 0, length, id}` followed by the text padded to 4. An id with no name,
 *        trace_id_overflow included, is resolved by the record's address, a register's by
 *        payload[1], a symbol's by payload[0]. Error offsets are the bytes durably written. An id
 * with no name, and names past 0xfffe distinct ones, stay trace_id_overflow. Names are cut at 65535
 * bytes.
 *
 * Neither copyable nor movable: sink() captures it. close() reports write errors, the destructor
 * closes and loses them.
 */
class pctr_writer
{
public:
  /**
   * @brief Creates the file and writes its header and origin.
   * @param path File.
   * @param origin Instruction count the first record's dt counts from.
   * @param names Names of register and symbol ids.
   * @throws format_error when the file cannot be created or written.
   */
  pctr_writer(std::filesystem::path path, icount origin, pctr_names names);

  /** @brief Not copyable. */
  pctr_writer(pctr_writer const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(pctr_writer const&) -> pctr_writer& = delete;
  /** @brief Not movable. */
  pctr_writer(pctr_writer&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(pctr_writer&&) -> pctr_writer& = delete;

  /** @brief Closes the file, a write error lost. */
  ~pctr_writer();

  /**
   * @brief Writes dt-stamped records, as a trace sink delivers them.
   * @param records Records, none of kind str, a contract checked in the body.
   * @throws format_error when the file cannot be written or is closed.
   */
  void write(std::span<core::trace_record const> records);

  /** @brief A trace sink writing into this writer, valid while it lives. */
  [[nodiscard]] auto sink() -> core::trace_sink;

  /**
   * @brief Writes what is buffered and closes the file. Does nothing when already closed.
   * @throws format_error when a write, the flush or the close fails.
   */
  void close();

  /** @brief Records written, STR and TIME included. */
  [[nodiscard]] auto records() const noexcept -> std::uint64_t;

  /** @brief Bytes accepted, header included: the file size once closed without error. */
  [[nodiscard]] auto bytes() const noexcept -> std::uint64_t;

private:
  struct closer
  {
    void operator()(std::FILE* file) const noexcept;
  };

  void put(void const* data, std::size_t size);
  void spill();
  void record(core::trace_record const& record);
  auto string_id(std::string const& name) -> std::uint16_t;
  auto resolve(core::trace_record const& record) -> std::uint16_t;

  std::filesystem::path                          path_;
  pctr_names                                     names_;
  std::unique_ptr<std::FILE, closer>             file_;
  std::vector<std::byte>                         buffer_;
  std::unordered_map<std::string, std::uint16_t> ids_;
  std::uint64_t                                  records_{};
  std::uint64_t                                  bytes_{};
  std::uint64_t                                  written_{};
};

/**
 * @brief A writer for a machine's trace: origin the machine's instruction count now, register
 *        names from its description, symbol names from its loaded images in load order. Create
 *        it right before add_trace_sink() and close it after the sink's handle is reset.
 * @param target Machine.
 * @param path File.
 * @return The writer.
 * @throws format_error when the file cannot be created.
 */
auto pctr_writer_for(core::machine const& target, std::filesystem::path path)
    -> std::unique_ptr<pctr_writer>;
}
