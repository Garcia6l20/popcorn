#include "elf_section.hpp"
#include "guard.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <dlfcn.h>
#include <link.h>
#include <sys/stat.h>

#include <algorithm>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace popcorn::plugin
{
namespace
{
struct file_id
{
  dev_t device;
  ino_t inode;

  auto operator<=>(file_id const&) const = default;
};

struct plugins
{
  std::mutex                       guard;
  std::deque<loaded>               records;
  std::map<file_id, loaded const*> by_file;
  std::map<void*, loaded const*>   by_handle;
  std::map<file_id, std::string>   poisoned;
  std::map<void*, std::string>     poisoned_handles;
  std::set<void*>                  opened;
};

auto plugins_table() -> plugins&
{
  static plugins instance;
  return instance;
}

using init_function = void (*)(registrar&);
using name_function = auto (*)() noexcept -> char const*;

char const anchor{};

constexpr std::string_view core_soname = "libpopcorn_core.so";

auto refusal(std::filesystem::path const& path, std::string key, std::string expected)
    -> config_error
{
  return config_error(path.string(), 0, std::move(key), std::move(expected));
}

auto own_symbol(void* handle, std::filesystem::path const& path, char const* name) -> void*
{
  auto* const symbol = dlsym(handle, name);
  if(symbol == nullptr)
  {
    throw refusal(path, name, std::format("a popcorn plugin defining {}", name));
  }
  link_map* own   = nullptr;
  void*     owner = nullptr;
  Dl_info   info{};
  if(dlinfo(handle, RTLD_DI_LINKMAP, &own) != 0
     or dladdr1(symbol, &info, &owner, RTLD_DL_LINKMAP) == 0
     or owner != own)
  {
    throw refusal(
        path, name,
        std::format("a popcorn plugin defining {} itself, not through a library it links", name));
  }
  return symbol;
}

auto canonical_of(std::filesystem::path const& path) -> std::filesystem::path
{
  std::error_code error;
  auto            found = std::filesystem::canonical(path, error);
  if(error)
  {
    throw refusal(path, "path", std::format("a plugin library, {}", error.message()));
  }
  return found;
}

void check_tag(std::filesystem::path const& path, std::string_view tag)
{
  if(tag != host_abi_tag())
  {
    throw refusal(
        path, "abi_tag",
        std::format("the tag of this popcorn \"{}\", not \"{}\"", host_abi_tag(), tag));
  }
}

void check_links(std::filesystem::path const& path)
{
  auto const links = read_dynamic_links(path);
  if(not links)
  {
    throw refusal(path, "link", "a plugin library with a readable dynamic section");
  }
  if(not std::ranges::contains(links->needed, core_soname))
  {
    throw refusal(
        path, "link",
        std::format(
            "a plugin linking {}, the library links its own copy of popcorn core", core_soname));
  }
  if(not links->registry_definitions.empty())
  {
    throw refusal(
        path, "link",
        std::format(
            "a plugin calling popcorn core's registry in {}, the library defines {}", core_soname,
            links->registry_definitions.front()));
  }
}

auto open(std::filesystem::path const& path, scope where) -> void*
{
  if(where == scope::declared)
  {
    guard::watch_constructors(path.native());
  }
  auto* const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
  if(handle == nullptr)
  {
    auto const* const reason = dlerror();
    throw refusal(path, "dlopen", reason != nullptr ? reason : "a loadable shared library");
  }
  return handle;
}

auto refused_by(std::string_view call) -> std::string
{
  return std::format(
      "a declared plugin to register its models with registrar::add_model, it called {}", call);
}

struct opened
{
  void*         handle;
  std::string   name;
  init_function init;
};

auto identity(std::filesystem::path const& path) -> file_id
{
  struct stat status{};
  if(::stat(path.c_str(), &status) != 0)
  {
    throw refusal(path, "path", "a plugin library that can be read");
  }
  return {.device = status.st_dev, .inode = status.st_ino};
}

auto checked_scope(loaded const& found, std::filesystem::path const& path, scope where)
    -> loaded const&
{
  if(found.where != where)
  {
    throw refusal(
        path, "scope",
        std::format("a {} load, the plugin is already loaded {}", where, found.where));
  }
  return found;
}

auto earlier_load(plugins& table, std::filesystem::path const& path, file_id id, scope where)
    -> loaded const*
{
  if(auto const poisoned = table.poisoned.find(id); poisoned != table.poisoned.end())
  {
    throw refusal(path, "static constructors", poisoned->second);
  }
  if(auto const found = table.by_file.find(id); found != table.by_file.end())
  {
    return &checked_scope(*found->second, path, where);
  }
  auto const tag = read_abi_section(path);
  if(not tag)
  {
    throw refusal(
        path, "abi_tag", "a popcorn plugin, the library has no .popcorn_abi section with a tag");
  }
  check_tag(path, *tag);
  check_links(path);
  auto* const probe = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if(probe == nullptr)
  {
    dlerror();
    return nullptr;
  }
  dlclose(probe);
  if(auto const poisoned = table.poisoned_handles.find(probe);
     poisoned != table.poisoned_handles.end())
  {
    throw refusal(path, "static constructors", poisoned->second);
  }
  if(auto const found = table.by_handle.find(probe); found != table.by_handle.end())
  {
    return &checked_scope(*found->second, path, where);
  }
  if(where == scope::builtin or table.opened.contains(probe))
  {
    return nullptr;
  }
  throw refusal(
      path, "dlopen",
      "a library only plugin::load maps, this one was mapped before and its static constructors "
      "ran unguarded");
}

auto open_checked(plugins& table, std::filesystem::path const& path, file_id id, scope where)
    -> opened
{
  auto* const handle = open(path, where);
  if(where == scope::declared)
  {
    table.opened.insert(handle);
  }
  if(auto const call = guard::violation(); not call.empty())
  {
    auto expected = refused_by(call);
    table.poisoned.emplace(id, expected);
    table.poisoned_handles.emplace(handle, expected);
    throw refusal(path, "static constructors", std::move(expected));
  }
  check_tag(path, static_cast<char const*>(own_symbol(handle, path, "popcorn_plugin_abi_tag")));
  return {
      .handle = handle,
      .name   = reinterpret_cast<name_function>(own_symbol(handle, path, "popcorn_plugin_name"))(),
      .init   = reinterpret_cast<init_function>(own_symbol(handle, path, "popcorn_plugin_init")),
  };
}
}

registrar::registrar(std::string name, scope where) : name_(std::move(name)), where_(where)
{}

void registrar::add_model(std::string_view name, periph::model_factory factory)
{
  if(std::ranges::contains(models_, name, &model_entry::name))
  {
    throw config_error(name_, 0, std::format("models.{}", name), "one registration per model name");
  }
  models_.push_back({.name = std::string{name}, .factory = std::move(factory)});
}

auto registrar::plugin_name() const noexcept -> std::string_view
{
  return name_;
}

auto library_directory() -> std::filesystem::path
{
  Dl_info         info{};
  std::error_code error;
  if(dladdr(&anchor, &info) != 0 and info.dli_fname != nullptr)
  {
    if(auto found = std::filesystem::canonical(info.dli_fname, error); not error)
    {
      return found.parent_path();
    }
  }
  throw config_error(
      "plugin", 0, "library_directory", "the dynamic loader to name popcorn core's object");
}

auto host_abi_tag() noexcept -> std::string_view
{
  return POPCORN_ABI_TAG_LITERAL;
}

auto find_model(loaded const& plugin, std::string_view name) -> periph::model_factory
{
  auto const found = std::ranges::find(plugin.models, name, &model_entry::name);
  return found != plugin.models.end() ? found->factory : periph::model_factory{};
}

auto load(std::filesystem::path const& path, scope where) -> loaded const&
{
  if(guard::in_load())
  {
    guard::violate("plugin::load");
    throw refusal(path, "plugin::load", "no plugin::load inside a plugin init");
  }
  guard::loading const  loading;
  auto&                 table = plugins_table();
  std::lock_guard const lock{table.guard};
  auto const            canonical_path = canonical_of(path);
  auto const            id             = identity(canonical_path);
  if(auto const* const found = earlier_load(table, canonical_path, id, where); found != nullptr)
  {
    return *found;
  }
  auto [handle, name, init] = open_checked(table, canonical_path, id, where);
  registrar plugin{name, where};
  if(where == scope::declared)
  {
    guard::watch_init(canonical_path.native());
  }
  init(plugin);
  if(auto const call = guard::violation(); not call.empty())
  {
    throw refusal(canonical_path, "popcorn_plugin_init", refused_by(call));
  }
  auto const& stored = table.records.emplace_back(
      loaded{
          .name    = std::move(name),
          .path    = canonical_path,
          .abi_tag = std::string{host_abi_tag()},
          .where   = where,
          .models  = std::move(plugin.models_),
      });
  table.by_file.emplace(id, &stored);
  table.by_handle.emplace(handle, &stored);
  return stored;
}
}
