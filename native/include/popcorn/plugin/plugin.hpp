#pragma once

/**
 * @file
 * @brief Native popcorn plugins: a shared library built against popcorn's C++ API, loaded at
 *        run time with plugin::load. A plugin defines its entry point with POPCORN_PLUGIN and
 *        records its models on the registrar it is handed. A declared plugin never writes the
 *        global model or arch registry: a call to periph::register_model or
 *        arch::register_arch from its static constructors or its init refuses the load.
 */

#include <popcorn/export.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/abi.hpp>

#include <reflex/enum.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::plugin
{
/**
 * @brief Where a plugin's registrations may go. A `builtin` is popcorn's own, its init may write
 *        the global registries. A `declared` plugin is one a machine description names, its
 *        models are named records only.
 */
enum class[[= reflex::derive{reflex::Format}]] scope : std::uint8_t
{
  builtin,
  declared
};

/** @brief One model a plugin provides, selected by name on the board. */
struct model_entry
{
  /** @brief Name the board selects the model by. */
  std::string name;
  /** @brief Factory of the model. */
  periph::model_factory factory;
};

struct loaded;

/**
 * @brief Loads a plugin once, never unloaded. A second call for the same library file, by any
 *        path or hard link to it, returns the first record. Every refusal happens before
 *        anything is recorded.
 * @param path Path of the plugin's shared library.
 * @param where scope::declared for a user plugin, scope::builtin for popcorn's own.
 * @return The record, valid for the life of the process.
 * @throws config_error naming `abi_tag` with both tags when the plugin was built for another
 *         popcorn, or naming `link` when the library has no `DT_NEEDED` on libpopcorn_core.so
 *         or defines periph::registered_glob or periph::register_model itself, a copy of core,
 *         both checked before `dlopen`, or naming the missing entry symbol, such as
 *         `popcorn_plugin_init`, when the library is not a popcorn plugin, or naming `dlopen`
 *         with the dlerror text when it cannot be opened, or, for scope::declared, when the
 *         library was mapped into the process other than by a declared load, or naming `scope`
 *         when it was loaded before with the other scope, or when a declared plugin calls a
 *         global registration, or when called from inside a plugin's static constructors or
 *         init. Exceptions of the plugin's init propagate. A scope::builtin library may be
 *         mapped before, by a program linking it.
 */
[[gnu::no_dangling]] POPCORN_API auto load(std::filesystem::path const& path, scope where)
    -> loaded const&;

/** @brief Handed to popcorn_plugin_init. Records what the plugin provides. */
class POPCORN_API registrar
{
public:
  registrar(registrar const&)                    = delete;
  auto operator=(registrar const&) -> registrar& = delete;

  /**
   * @brief Records a model under @p name.
   * @param name Name the board selects the model by.
   * @param factory Factory of the model.
   * @throws config_error when @p name is already registered by this plugin.
   */
  void add_model(std::string_view name, periph::model_factory factory)
      pre(not name.empty() and static_cast<bool>(factory));

  /** @brief The plugin's name, as POPCORN_PLUGIN spells it. */
  [[nodiscard]] auto plugin_name() const noexcept -> std::string_view;

private:
  friend auto load(std::filesystem::path const& path, scope where) -> loaded const&;

  registrar(std::string name, scope where);

  std::string              name_;
  scope                    where_;
  std::vector<model_entry> models_;
};

/** @brief A loaded plugin. Never unloaded. */
struct loaded
{
  /** @brief The plugin's name, as POPCORN_PLUGIN spells it. */
  std::string name;
  /** @brief Canonical path of the library. */
  std::filesystem::path path;
  /** @brief The ABI tag the plugin embeds, equal to host_abi_tag(). */
  std::string abi_tag;
  /** @brief The scope it was loaded with. */
  scope where;
  /** @brief The models its init recorded, in order. */
  std::vector<model_entry> models;
};

/**
 * @brief The model @p plugin records under @p name.
 * @return Its factory, empty when the plugin records none of that name.
 */
[[nodiscard]] POPCORN_API auto find_model(loaded const& plugin, std::string_view name)
    -> periph::model_factory;

/**
 * @brief The directory of `libpopcorn_core.so`, where the built-in plugins
 *        `libpopcorn_armv8m.so` and `libpopcorn_stm32.so` sit beside it. In a program linking
 *        the static core, the dynamic loader names the program by its `argv[0]`: the result is
 *        the program's directory only when `argv[0]` is a path to it, and a bare name found
 *        through `PATH` resolves against the current directory.
 * @throws config_error naming `library_directory` when the dynamic loader cannot name it, or
 *         when the name it gives does not resolve.
 */
[[nodiscard]] POPCORN_API auto library_directory() -> std::filesystem::path;

/** @brief This build's ABI tag, the string every plugin must embed. */
[[nodiscard]] POPCORN_API auto host_abi_tag() noexcept -> std::string_view;
}

/**
 * @brief Defines a plugin's ABI tag, its name @p NAME and its entry point, whose body follows
 *        the macro and receives the registrar as @p REGISTRAR.
 *
 * @code
 * POPCORN_PLUGIN(my_plugin, r)
 * {
 *   r.add_model("rng", make_rng);
 * }
 * @endcode
 */
#define POPCORN_PLUGIN(NAME, REGISTRAR)                                               \
  extern "C" [[gnu::visibility("default"), gnu::used, gnu::section(".popcorn_abi")]]  \
  char const popcorn_plugin_abi_tag[] = POPCORN_ABI_TAG_LITERAL;                      \
  extern "C" [[gnu::visibility("default")]] char const popcorn_plugin_name[] = #NAME; \
  extern "C" [[gnu::visibility("default")]] void       popcorn_plugin_init(           \
      ::popcorn::plugin::registrar& REGISTRAR)
