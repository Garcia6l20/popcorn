#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <format>

namespace popcorn::core
{
machine_services::~machine_services() = default;

void machine_services::do_watch_reads(address at, std::uint32_t size, bool)
{
  throw config_error(
      "machine_services", 0, std::format("watch_reads at {:#010x}, {} bytes", at, size),
      "an implementation with controller regions");
}
}
