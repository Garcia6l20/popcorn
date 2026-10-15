#pragma once

#include <doctest/doctest.h>

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace plugin_test
{
/** @brief Directory of the fixture plugins, from POPCORN_PLUGIN_FIXTURES. */
inline auto fixtures() -> std::filesystem::path
{
  auto const* const dir = std::getenv("POPCORN_PLUGIN_FIXTURES");
  REQUIRE(dir != nullptr);
  return dir;
}

/** @brief The fixture plugin @p name as built. */
inline auto fixture(std::string_view name) -> std::filesystem::path
{
  return fixtures() / std::format("{}.so", name);
}

/** @brief Prefix of the temporary directories of copies. */
inline constexpr std::string_view copies_prefix = "popcorn-plugin-test-";

/** @brief Removes the copies directories of processes that no longer run, left by a case that
 * aborted or was killed before its destructors ran. */
inline void sweep_dead_copies()
{
  std::error_code ignored;
  for(auto const& entry :
      std::filesystem::directory_iterator{std::filesystem::temp_directory_path(), ignored})
  {
    auto const name = entry.path().filename().string();
    if(not name.starts_with(copies_prefix))
    {
      continue;
    }
    auto const rest         = std::string_view{name}.substr(copies_prefix.size());
    pid_t      pid          = 0;
    auto const [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), pid);
    if(error == std::errc{}
       and end != rest.data()
       and pid > 0
       and kill(pid, 0) != 0
       and errno == ESRCH)
    {
      std::filesystem::remove_all(entry.path(), ignored);
    }
  }
}

/** @brief Copies of fixtures at fresh paths, removed with the object, and by the next process's
 * sweep when this one dies first. A fresh path is a plugin no case of this process loaded
 * before. */
class copies
{
public:
  copies()
      : dir_(
            std::filesystem::temp_directory_path()
            / std::format("{}{}-{}", copies_prefix, getpid(), next()))
  {
    sweep_dead_copies();
    std::filesystem::create_directories(dir_);
  }

  ~copies()
  {
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  copies(copies const&)                    = delete;
  auto operator=(copies const&) -> copies& = delete;

  /** @brief A fresh copy of the fixture @p name. */
  auto of(std::string_view name) -> std::filesystem::path
  {
    auto target = dir_ / std::format("{}-{}.so", name, count_++);
    std::filesystem::copy_file(fixture(name), target);
    return target;
  }

  /** @brief A new hard link to @p path in this object's directory. */
  auto hard_link(std::filesystem::path const& path) -> std::filesystem::path
  {
    auto target = dir_ / std::format("link-{}.so", count_++);
    std::filesystem::create_hard_link(path, target);
    return target;
  }

  /** @brief The directory holding the copies. */
  [[nodiscard]] auto dir() const -> std::filesystem::path const&
  {
    return dir_;
  }

private:
  static auto next() -> int
  {
    static std::atomic<int> made{0};
    return made++;
  }

  std::filesystem::path dir_;
  int                   count_ = 0;
};

/** @brief How often the init of the loaded `good` copy at @p path ran. */
inline auto init_calls(std::filesystem::path const& path) -> int
{
  auto* const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  REQUIRE(handle != nullptr);
  auto const* const calls = static_cast<std::atomic<int> const*>(dlsym(handle, "good_init_calls"));
  REQUIRE(calls != nullptr);
  auto const value = calls->load();
  dlclose(handle);
  return value;
}

/** @brief Whether the library at @p path is loaded in this process. */
inline auto is_loaded(std::filesystem::path const& path) -> bool
{
  auto* const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if(handle == nullptr)
  {
    return false;
  }
  dlclose(handle);
  return true;
}
}
