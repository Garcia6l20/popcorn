#include <popcorn/config/project.hpp>

#include <popcorn/core/errors.hpp>

#include <reflex/hash/sha256.hpp>
#include <reflex/meta.hpp>
#include <reflex/serde/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <meta>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::config
{
namespace
{
namespace toml = reflex::serde::toml;

template <typename T> inline constexpr bool is_optional                      = false;
template <typename T> inline constexpr bool is_optional<std::optional<T>>    = true;
template <typename T> inline constexpr bool is_vector                        = false;
template <typename T> inline constexpr bool is_vector<std::vector<T>>        = true;
template <typename T> inline constexpr bool is_map                           = false;
template <typename T> inline constexpr bool is_map<std::map<std::string, T>> = true;

template <typename T>
concept table_type = std::is_class_v<T>
                 and not std::same_as<T, std::string>
                 and not std::same_as<T, toml::value>
                 and not is_optional<T>
                 and not is_vector<T>
                 and not is_map<T>;

auto trim(std::string_view text) -> std::string_view
{
  auto const first = text.find_first_not_of(" \t\r");
  if(first == std::string_view::npos)
  {
    return {};
  }
  return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

auto without_comment(std::string_view line) -> std::string_view
{
  char quote = 0;
  for(std::size_t i = 0; i < line.size(); ++i)
  {
    auto const c = line[i];
    if(quote == '"' and c == '\\')
    {
      ++i;
    }
    else if(quote != 0)
    {
      if(c == quote)
      {
        quote = 0;
      }
    }
    else if(c == '"' or c == '\'')
    {
      quote = c;
    }
    else if(c == '#')
    {
      return line.substr(0, i);
    }
  }
  return line;
}

auto bare_key_char(char c) -> bool
{
  return std::isalnum(static_cast<unsigned char>(c)) != 0 or c == '_' or c == '-';
}

auto normal_key(std::string_view key) -> std::optional<std::string>
{
  std::vector<std::string>   segments;
  std::string                bare;
  std::optional<std::string> quoted;
  char                       quote  = 0;
  auto const                 finish = [&] {
    auto const plain = std::string{trim(bare)};
    if(quoted ? not plain.empty() : plain.empty())
    {
      return false;
    }
    segments.push_back(quoted ? *quoted : plain);
    bare.clear();
    quoted.reset();
    return true;
  };
  for(auto const c : key)
  {
    if(quote != 0)
    {
      if(c == quote)
      {
        quote = 0;
      }
      else
      {
        *quoted += c;
      }
    }
    else if(c == '"' or c == '\'')
    {
      if(quoted or not trim(bare).empty())
      {
        return std::nullopt;
      }
      quote  = c;
      quoted = std::string{};
    }
    else if(c == '.')
    {
      if(not finish())
      {
        return std::nullopt;
      }
    }
    else if(c == ' ' or c == '\t')
    {
      if(not quoted)
      {
        bare += ' ';
      }
    }
    else if(quoted or not bare_key_char(c))
    {
      return std::nullopt;
    }
    else
    {
      bare += c;
    }
  }
  if(quote != 0 or not finish())
  {
    return std::nullopt;
  }
  return segments | std::views::join_with('.') | std::ranges::to<std::string>();
}

auto join(std::string_view base, std::string_view key) -> std::string
{
  return base.empty() ? std::string{key} : std::format("{}.{}", base, key);
}

auto without_indices(std::string_view key) -> std::string
{
  std::string out;
  bool        inside = false;
  for(auto const c : key)
  {
    if(c == '[')
    {
      inside = true;
    }
    else if(c == ']')
    {
      inside = false;
    }
    else if(not inside)
    {
      out += c;
    }
  }
  return out;
}

auto line_in(std::map<std::string, std::uint32_t> const& lines, std::string_view path)
    -> std::uint32_t
{
  for(auto key : {std::string{path}, without_indices(path)})
  {
    while(not key.empty())
    {
      if(auto const found = lines.find(key); found != lines.end())
      {
        return found->second;
      }
      auto const cut = key.find_last_of(".[");
      key            = cut == std::string::npos ? std::string{} : key.substr(0, cut);
    }
  }
  return 0;
}

auto count_of(std::string_view line, std::string_view delimiter) -> std::size_t
{
  std::size_t count = 0;
  for(auto at = line.find(delimiter); at != std::string_view::npos;
      at      = line.find(delimiter, at + delimiter.size()))
  {
    ++count;
  }
  return count;
}

constexpr std::array<std::string_view, 2> multi_line_quotes{R"(""")", "'''"};

class key_lines
{
public:
  explicit key_lines(std::string_view text)
  {
    std::string                        table;
    std::map<std::string, std::size_t> arrays;
    std::string_view                   closing;
    std::uint32_t                      number = 0;
    for(auto const raw_range : text | std::views::split('\n'))
    {
      ++number;
      auto const raw = std::string_view{raw_range.begin(), raw_range.end()};
      if(not closing.empty())
      {
        if(count_of(raw, closing) % 2 == 1)
        {
          closing = {};
        }
        continue;
      }
      for(auto const delimiter : multi_line_quotes)
      {
        if(count_of(raw, delimiter) % 2 == 1)
        {
          closing = delimiter;
        }
      }
      auto const line = trim(without_comment(raw));
      if(line.starts_with("[["))
      {
        if(auto const name = normal_key(trim(line.substr(2, line.find("]]") - 2))))
        {
          record(*name, number);
          table = std::format("{}[{}]", *name, arrays[*name]++);
          record(table, number);
        }
        continue;
      }
      if(line.starts_with('['))
      {
        if(auto const name = normal_key(trim(line.substr(1, line.find(']') - 1))))
        {
          table = *name;
          record(table, number);
        }
        continue;
      }
      auto const equals = line.find('=');
      if(equals == std::string_view::npos)
      {
        continue;
      }
      if(auto const key = normal_key(trim(line.substr(0, equals))))
      {
        record(join(table, *key), number);
      }
    }
  }

  [[nodiscard]] auto line_of(std::string const& path) const -> std::uint32_t
  {
    return line_in(lines_, path);
  }

  [[nodiscard]] auto lines() const -> std::map<std::string, std::uint32_t> const&
  {
    return lines_;
  }

  [[nodiscard]] auto order() const -> std::vector<std::string> const&
  {
    return order_;
  }

private:
  void record(std::string const& path, std::uint32_t number)
  {
    if(lines_.emplace(path, number).second)
    {
      order_.push_back(path);
    }
  }

  std::map<std::string, std::uint32_t> lines_;
  std::vector<std::string>             order_;
};

struct context
{
  std::string      file;
  key_lines const* lines;

  [[noreturn]] void fail(std::string const& key, std::string expected) const
  {
    throw config_error(file, lines->line_of(key), key, std::move(expected));
  }
};

template <typename T>
auto decode(toml::value const& value, std::string const& path, context const& ctx) -> T;

template <typename T> consteval auto decoded_members()
{
  std::vector<std::meta::info> out;
  for(auto const member :
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()))
  {
    if(not reflex::meta::has_annotation(member, ^^computed_t)
       and not reflex::meta::has_annotation(member, ^^extra_keys_t))
    {
      out.push_back(member);
    }
  }
  return std::define_static_array(out);
}

template <typename T> consteval auto extra_member() -> std::meta::info
{
  for(auto const member :
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()))
  {
    if(reflex::meta::has_annotation(member, ^^extra_keys_t))
    {
      return member;
    }
  }
  return std::meta::info{};
}

template <typename T> auto key_names() -> std::string
{
  std::string names;
  template for(constexpr auto member : decoded_members<T>())
  {
    names += names.empty() ? "" : ", ";
    names += std::meta::identifier_of(member);
  }
  return names;
}

template <table_type T>
auto decode_table(toml::value const& value, std::string const& path, context const& ctx) -> T
{
  if(not value.is_object())
  {
    ctx.fail(path, "a table");
  }
  auto const&           table = value.as<toml::object>();
  T                     out{};
  std::set<std::string> used;
  template for(constexpr auto member : decoded_members<T>())
  {
    using member_type               = typename[:std::meta::type_of(member):];
    constexpr std::string_view name = std::meta::identifier_of(member);
    auto const                 key  = join(path, name);
    if(auto const found = table.find(std::string{name}); found != table.end())
    {
      out.[:member:] = decode<member_type>(found->second, key, ctx);
      used.emplace(name);
    }
    else if constexpr(reflex::meta::has_annotation(member, ^^required_t))
    {
      ctx.fail(key, "a value, required");
    }
    else if constexpr(table_type<member_type>)
    {
      out.[:member:] = decode<member_type>(toml::value{toml::object{}}, key, ctx);
    }
  }
  for(auto const& [key, item] : table)
  {
    if(used.contains(key))
    {
      continue;
    }
    constexpr auto extra = extra_member<T>();
    if constexpr(extra != std::meta::info{})
    {
      out.[:extra:].emplace(key, item);
    }
    else
    {
      ctx.fail(
          join(path, key),
          std::format("a key of {}: {}", path.empty() ? "the project file" : path, key_names<T>()));
    }
  }
  return out;
}

template <typename T>
auto decode(toml::value const& value, std::string const& path, context const& ctx) -> T
{
  if constexpr(std::same_as<T, toml::value>)
  {
    return value;
  }
  else if constexpr(is_optional<T>)
  {
    return decode<typename T::value_type>(value, path, ctx);
  }
  else if constexpr(std::same_as<T, std::string>)
  {
    if(not value.is<toml::string>())
    {
      ctx.fail(path, "a string");
    }
    return value.as<toml::string>();
  }
  else if constexpr(std::same_as<T, bool>)
  {
    if(not value.is<toml::boolean>())
    {
      ctx.fail(path, "true or false");
    }
    return value.as<toml::boolean>();
  }
  else if constexpr(std::is_integral_v<T>)
  {
    constexpr auto most = std::numeric_limits<T>::max();
    if(not value.is<toml::integer>()
       or value.as<toml::integer>()
       < 0
       or static_cast<std::uint64_t>(value.as<toml::integer>())
       > most)
    {
      ctx.fail(path, std::format("an integer from 0 to {}", most));
    }
    return static_cast<T>(value.as<toml::integer>());
  }
  else if constexpr(std::is_floating_point_v<T>)
  {
    if(value.is<toml::integer>())
    {
      return static_cast<T>(value.as<toml::integer>());
    }
    if(not value.is<toml::number>())
    {
      ctx.fail(path, "a number");
    }
    return static_cast<T>(value.as<toml::number>());
  }
  else if constexpr(is_vector<T>)
  {
    if(not value.is_array())
    {
      ctx.fail(path, "an array");
    }
    T out;
    for(auto const& [index, item] : std::views::enumerate(value.as<toml::array>()))
    {
      out.push_back(decode<typename T::value_type>(item, std::format("{}[{}]", path, index), ctx));
    }
    return out;
  }
  else if constexpr(is_map<T>)
  {
    if(not value.is_object())
    {
      ctx.fail(path, "a table");
    }
    T out;
    for(auto const& [key, item] : value.as<toml::object>())
    {
      out.emplace(key, decode<typename T::mapped_type>(item, join(path, key), ctx));
    }
    return out;
  }
  else
  {
    return decode_table<T>(value, path, ctx);
  }
}

auto read_text(std::filesystem::path const& file) -> std::string
{
  std::ifstream stream{file, std::ios::binary};
  if(not stream)
  {
    throw config_error(file.string(), 0, "<file>", "a readable project file");
  }
  return {std::istreambuf_iterator<char>{stream}, {}};
}

auto parse(std::string const& text, std::string const& file) -> toml::value
{
  toml::value document;
  try
  {
    document = toml::deserializer{text}.load<toml::value>();
  }
  catch(std::exception const& failure)
  {
    throw config_error(file, 0, "<document>", std::format("valid TOML: {}", failure.what()));
  }
  if(not document.is_object())
  {
    throw config_error(file, 0, "<document>", "a TOML table");
  }
  return document;
}

void overlay(
    toml::value&              target,
    toml::value const&        source,
    std::string const&        path,
    std::vector<std::string>& leaves)
{
  for(auto const& [key, item] : source.as<toml::object>())
  {
    auto const full  = join(path, key);
    auto&      table = target.as<toml::object>();
    auto const found = table.find(key);
    if(item.is_object() and found != table.end() and found->second.is_object())
    {
      overlay(found->second, item, full, leaves);
      continue;
    }
    table.insert_or_assign(key, item);
    leaves.push_back(full);
  }
}

auto config_names(toml::value const& configs) -> std::vector<std::string>
{
  return configs.as<toml::object>()
       | std::views::keys
       | std::ranges::to<std::vector<std::string>>();
}

auto dump(toml::value const& document) -> std::string
{
  std::string      text;
  toml::serializer serializer{std::back_inserter(text)};
  serializer.dump(document);
  return text;
}

void resolve(std::optional<std::string>& path, std::filesystem::path const& base)
{
  if(path and std::filesystem::path{*path}.is_relative())
  {
    path = (base / *path).lexically_normal().string();
  }
}

void resolve(std::string& path, std::filesystem::path const& base)
{
  if(std::filesystem::path{path}.is_relative())
  {
    path = (base / path).lexically_normal().string();
  }
}

void check_values(project_file const& file, context const& ctx)
{
  if(not std::isfinite(file.clock.cpi) or not(file.clock.cpi > 0.0))
  {
    ctx.fail("clock.cpi", "a finite number of cycles per instruction above 0");
  }
  if(auto const& base = file.boot.arena_base; base)
  {
    auto const address = base->is<toml::integer>()
                     and base->as<toml::integer>()
                      >= 0
                     and base->as<toml::integer>()
                      <= 0xffff'ffff;
    auto const disabled = base->is<toml::boolean>() and not base->as<toml::boolean>();
    if(not address and not disabled)
    {
      ctx.fail("boot.arena_base", "an address from 0 to 0xffffffff, or false for no arena");
    }
  }
  if(file.clock.core_hz == 0u)
  {
    ctx.fail("clock.core_hz", "a core clock above 0 Hz");
  }
  for(auto const& [name, entry] : file.board)
  {
    if(not entry.model and not entry.params.empty())
    {
      ctx.fail(
          std::format("board.{}.{}", name, entry.params.begin()->first),
          std::format(
              "a key of board.{}: connect, model, devices, or a model parameter with model set",
              name));
    }
  }
}

void check_images(project_file const& file, context const& ctx)
{
  for(auto const& [name, entry] : file.image)
  {
    auto const key   = join("image", name);
    auto const files = static_cast<int>(entry.elf.has_value())
                     + static_cast<int>(entry.bin.has_value())
                     + static_cast<int>(entry.hex.has_value());
    if(files != 1)
    {
      ctx.fail(key, "exactly one of elf, bin and hex");
    }
    if(entry.bin.has_value() != entry.at.has_value())
    {
      ctx.fail(join(key, "at"), "a load address with bin, and only with bin");
    }
  }
  for(auto const& [index, init] : std::views::enumerate(file.boot.memory_init))
  {
    auto const& bytes = init.bytes;
    if(bytes.size() % 2 != 0 or not std::ranges::all_of(bytes, [](char c) {
         return std::isxdigit(static_cast<unsigned char>(c)) != 0;
       }))
    {
      ctx.fail(
          std::format("boot.memory_init[{}].bytes", index), "hexadecimal digits, two per byte");
    }
  }
}

auto image_order(project_file const& file, key_lines const& lines, context const& ctx)
    -> std::vector<std::string>
{
  std::vector<std::string> order;
  for(auto const& path : lines.order())
  {
    if(not path.starts_with("image."))
    {
      continue;
    }
    auto const rest = std::string_view{path}.substr(6);
    auto const name = std::string{rest.substr(0, rest.find('.'))};
    if(file.image.contains(name) and std::ranges::find(order, name) == order.end())
    {
      order.push_back(name);
    }
  }
  auto const unplaced = std::ranges::count_if(file.image | std::views::keys, [&](auto const& name) {
    return std::ranges::find(order, name) == order.end();
  });
  if(unplaced >= 2)
  {
    ctx.fail(
        "image",
        "each image declared as [image.<name>] or image.<name>.<key> lines, for load order");
  }
  for(auto const& name : file.image | std::views::keys)
  {
    if(std::ranges::find(order, name) == order.end())
    {
      order.push_back(name);
    }
  }
  return order;
}
}

auto load_project(std::filesystem::path const& file, std::optional<std::string_view> config)
    -> loaded_project
{
  auto const      absolute = std::filesystem::absolute(file).lexically_normal();
  auto const      text     = read_text(absolute);
  auto const      name     = absolute.string();
  key_lines const lines{text};
  context const   ctx{.file = name, .lines = &lines};

  auto           document = parse(text, name);
  loaded_project out{
      .file = {}, .path = name, .configs = {}, .config = {}, .digest = {}, .lines = {}};
  toml::value configs{toml::object{}};
  auto&       root = document.as<toml::object>();
  if(auto const found = root.find("configs"); found != root.end())
  {
    configs = found->second;
    root.erase(found);
    if(not configs.is_object())
    {
      ctx.fail("configs", "a table of named configs");
    }
    for(auto const& [config_name, overlay_table] : configs.as<toml::object>())
    {
      if(not overlay_table.is_object())
      {
        ctx.fail(join("configs", config_name), "a table of dotted keys");
      }
    }
  }
  out.configs = config_names(configs);

  std::vector<std::string> leaves;
  if(config)
  {
    auto const& named = configs.as<toml::object>();
    auto const  found = named.find(std::string{*config});
    if(found == named.end())
    {
      auto const known = out.configs
                       | std::views::join_with(std::string_view{", "})
                       | std::ranges::to<std::string>();
      ctx.fail(
          join("configs", *config),
          std::format("a config of the project file: {}", known.empty() ? "none" : known));
    }
    overlay(document, found->second, {}, leaves);
    out.config = std::string{*config};
  }

  try
  {
    out.file = decode<project_file>(document, {}, ctx);
  }
  catch(config_error const& failure)
  {
    auto const from_config = std::ranges::find_if(leaves, [&](std::string const& leaf) {
      return failure.key()
          == leaf
          or failure.key().starts_with(leaf + ".")
          or failure.key().starts_with(leaf + "[");
    });
    if(from_config == leaves.end())
    {
      throw;
    }
    auto const key = join(join("configs", *config), failure.key());
    throw config_error(name, lines.line_of(key), key, failure.expected());
  }
  check_values(out.file, ctx);
  check_images(out.file, ctx);

  auto const base = absolute.parent_path();
  resolve(out.file.mcu.ioc, base);
  resolve(out.file.mcu.description, base);
  if(out.file.mcu.svd != "auto")
  {
    resolve(out.file.mcu.svd, base);
  }
  for(auto& entry : out.file.image | std::views::values)
  {
    resolve(entry.elf, base);
    resolve(entry.bin, base);
    resolve(entry.hex, base);
  }
  for(auto& directory : out.file.models.python)
  {
    resolve(directory, base);
  }
  out.file.image_order = image_order(out.file, lines, ctx);
  out.lines            = lines.lines();

  auto const digest = reflex::sha256_of(dump(document));
  auto const hex    = reflex::to_hex(digest);
  out.digest        = std::string{hex.begin(), hex.end()};
  return out;
}

auto loaded_project::line_of(std::string const& key) const -> std::uint32_t
{
  return line_in(lines, key);
}
}
