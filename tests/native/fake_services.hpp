#pragma once

#include <popcorn/core/services.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::testing
{
/**
 * @brief machine_services fake for tests: sparse little-endian byte memory, writes below
 *        read_only_below denied, requests recorded.
 */
class fake_services final : public core::machine_services
{
public:
  /** @brief Guest bytes by address, absent bytes are unmapped. */
  std::map<address, std::byte> memory;
  /** @brief Writes below this address are denied. */
  address read_only_below{};
  /** @brief Last stop request. */
  std::optional<std::pair<stop_reason, std::string>> stop;
  /** @brief Last reset request. */
  std::optional<reset_kind> reset;
  /** @brief Number of end_slice() calls. */
  std::uint32_t slice_ends{};
  /** @brief Number of end_after_instruction() calls. */
  std::uint32_t instruction_ends{};
  /** @brief Bus master of the last access. */
  std::optional<initiator> last_by;
  /** @brief Records passed to log(), in order. */
  std::vector<core::log_record> logged;

  /**
   * @brief Stores a little-endian word, bypassing the bus.
   * @param addr First byte address.
   * @param value Word to store.
   */
  void poke_word(address addr, std::uint32_t value)
  {
    for(std::uint32_t index = 0; index < 4; ++index)
    {
      memory[addr + index] = static_cast<std::byte>(value >> (8 * index));
    }
  }

private:
  void do_request_stop(stop_reason reason, std::string detail) override
  {
    stop.emplace(reason, std::move(detail));
  }

  void do_request_reset(reset_kind kind) override
  {
    reset = kind;
  }

  void do_end_slice() override
  {
    ++slice_ends;
  }

  void do_end_after_instruction() override
  {
    ++instruction_ends;
  }

  auto do_read(address addr, std::uint8_t size, initiator by)
      -> std::expected<std::uint32_t, bus_error> override
  {
    last_by             = by;
    std::uint32_t value = 0;
    for(std::uint32_t index = 0; index < size; ++index)
    {
      auto const found = memory.find(addr + index);
      if(found == memory.end())
      {
        return std::unexpected{bus_error::unmapped};
      }
      value |= std::to_integer<std::uint32_t>(found->second) << (8 * index);
    }
    return value;
  }

  auto do_write(address addr, std::uint8_t size, std::uint32_t value, initiator by)
      -> std::expected<void, bus_error> override
  {
    last_by = by;
    if(addr < read_only_below)
    {
      return std::unexpected{bus_error::denied};
    }
    for(std::uint32_t index = 0; index < size; ++index)
    {
      memory[addr + index] = static_cast<std::byte>(value >> (8 * index));
    }
    return {};
  }

  auto do_read_block(address addr, std::span<std::byte> out, initiator by)
      -> std::expected<void, core::block_error> override
  {
    for(std::size_t done = 0; done < out.size();)
    {
      auto const at   = addr + static_cast<address>(done);
      auto const size = core::block_access_size(at, out.size() - done);
      auto const word = do_read(at, size, by);
      if(not word)
      {
        return std::unexpected{
            core::block_error{word.error(), at}
        };
      }
      for(std::uint8_t index = 0; index < size; ++index)
      {
        out[done + index] = static_cast<std::byte>(*word >> (8 * index));
      }
      done += size;
    }
    return {};
  }

  auto do_write_block(address addr, std::span<std::byte const> in, initiator by)
      -> std::expected<void, core::block_error> override
  {
    for(std::size_t done = 0; done < in.size();)
    {
      auto const    at   = addr + static_cast<address>(done);
      auto const    size = core::block_access_size(at, in.size() - done);
      std::uint32_t word = 0;
      for(std::uint8_t index = 0; index < size; ++index)
      {
        word |= std::to_integer<std::uint32_t>(in[done + index]) << (8 * index);
      }
      auto const written = do_write(at, size, word, by);
      if(not written)
      {
        return std::unexpected{
            core::block_error{written.error(), at}
        };
      }
      done += size;
    }
    return {};
  }

  void do_trace(core::trace_record const&) override
  {}

  void do_log(core::log_record record) override
  {
    logged.push_back(std::move(record));
  }
};
}
