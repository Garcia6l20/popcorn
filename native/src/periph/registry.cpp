#include "../plugin/guard.hpp"
#include "register_layer.hpp"

#include <popcorn/core/services.hpp>
#include <popcorn/periph/model.hpp>

#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::periph
{
model::~model()                   = default;
peer_directory::~peer_directory() = default;

auto model::quiet_until(quiet_query const& query) const -> std::optional<vtime>
{
  return query.next_event(*this);
}

namespace
{
struct registration
{
  std::string   ip;
  std::string   glob;
  model_factory factory;
};

struct registry
{
  std::mutex                guard;
  std::vector<registration> entries;
};

auto table() -> registry&
{
  static registry instance;
  return instance;
}
}

auto glob_match(std::string_view pattern, std::string_view text) noexcept -> bool
{
  if(pattern.empty())
  {
    return text.empty();
  }
  if(pattern.front() == '*')
  {
    for(std::size_t skip = 0; skip <= text.size(); ++skip)
    {
      if(glob_match(pattern.substr(1), text.substr(skip)))
      {
        return true;
      }
    }
    return false;
  }
  if(text.empty())
  {
    return false;
  }
  if(pattern.front() == '?' or pattern.front() == text.front())
  {
    return glob_match(pattern.substr(1), text.substr(1));
  }
  return false;
}

void register_model(std::string_view ip, std::string_view ip_version_glob, model_factory factory)
{
  if(plugin::guard::refuses("periph::register_model"))
  {
    return;
  }
  auto&                 entries = table();
  std::lock_guard const lock{entries.guard};
  std::erase_if(entries.entries, [&](registration const& entry) {
    return entry.ip == ip and entry.glob == ip_version_glob;
  });
  if(not factory)
  {
    return;
  }
  entries.entries.push_back(
      {.ip = std::string{ip}, .glob = std::string{ip_version_glob}, .factory = std::move(factory)});
}

namespace
{
auto latest(std::string_view ip, std::string_view ip_version) -> registration const*
{
  for(auto const& entry : table().entries | std::views::reverse)
  {
    if(entry.ip == ip and glob_match(entry.glob, ip_version))
    {
      return &entry;
    }
  }
  return nullptr;
}
}

auto find_model(peripheral_desc const& desc) -> model_factory
{
  std::lock_guard const lock{table().guard};
  auto const* const     entry = latest(desc.ip, desc.ip_version);
  return entry != nullptr ? entry->factory : model_factory{};
}

auto registered_glob(std::string_view ip, std::string_view ip_version) -> std::optional<std::string>
{
  std::lock_guard const lock{table().guard};
  auto const* const     entry = latest(ip, ip_version);
  return entry != nullptr ? std::optional{entry->glob} : std::nullopt;
}

void model_context::request_stop(stop_reason reason, std::string detail)
{
  services.request_stop(reason, std::move(detail));
}

void model_context::install_firewall(bus_firewall& firewall)
{
  services.install_firewall(firewall);
}
}
