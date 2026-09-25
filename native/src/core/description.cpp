#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>

#include <reflex/hash/sha256.hpp>
#include <reflex/serde/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <meta>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>

namespace popcorn
{
namespace
{
namespace json = reflex::serde::json;

[[noreturn]] void throw_version_mismatch(std::string_view source, auto found)
{
  throw config_error(
      std::string(source), 0, "schema_version",
      std::format("schema version {}, file has {}", description_schema_version, found));
}

[[noreturn]] void throw_version_missing(std::string_view source)
{
  throw config_error(
      std::string(source), 0, "schema_version",
      std::format("schema version {}, file has no schema_version", description_schema_version));
}

void refuse_other_version(std::string_view text, std::string_view source)
{
  std::optional<json::value> document;
  try
  {
    document = json::deserializer{text}.load<json::value>();
  }
  catch(std::exception const&)
  {
    return;
  }
  if(not document->is_object())
  {
    return;
  }
  auto const& object  = document->as_object();
  auto const  version = object.find("schema_version");
  if(version == object.end())
  {
    throw_version_missing(source);
  }
  if(version->second.is<json::number>()
     and version->second.as<json::number>()
     != static_cast<json::number>(description_schema_version))
  {
    throw_version_mismatch(source, version->second.as<json::number>());
  }
}

template <typename T> auto schema_of(json::object& defs) -> json::value
{
  if constexpr(std::same_as<T, bool>)
  {
    return {
        {"type", "boolean"}
    };
  }
  else if constexpr(
      std::integral<T>
      and std::numeric_limits<T>::digits
      <= std::numeric_limits<json::number>::digits)
  {
    return {
        {"type",    "integer"                                               },
        {"minimum", static_cast<json::number>(std::numeric_limits<T>::min())},
        {"maximum", static_cast<json::number>(std::numeric_limits<T>::max())}
    };
  }
  else if constexpr(std::unsigned_integral<T>)
  {
    return {
        {"type",    "integer"},
        {"minimum", 0.0      }
    };
  }
  else if constexpr(std::same_as<T, std::string>)
  {
    return {
        {"type", "string"}
    };
  }
  else if constexpr(reflex::enum_c<T>)
  {
    json::array names;
    template for(constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^T)))
    {
      names.emplace_back(std::string(std::meta::identifier_of(e)));
    }
    return {
        {"type", "string"        },
        {"enum", std::move(names)}
    };
  }
  else if constexpr(reflex::optional_c<T>)
  {
    return schema_of<typename T::value_type>(defs);
  }
  else if constexpr(reflex::map_c<T>)
  {
    static_assert(std::same_as<typename T::key_type, std::string>, "JSON object keys are strings");
    return {
        {"type",                 "object"                                },
        {"additionalProperties", schema_of<typename T::mapped_type>(defs)}
    };
  }
  else if constexpr(reflex::seq_c<T>)
  {
    return {
        {"type",  "array"                                },
        {"items", schema_of<typename T::value_type>(defs)}
    };
  }
  else
  {
    static_assert(reflex::aggregate_c<T>, "no JSON Schema mapping for this description member");
    constexpr std::string_view name = std::meta::identifier_of(^^T);
    if(not defs.contains(std::string(name)))
    {
      json::object properties;
      json::array  required;
      template for(constexpr auto member : std::define_static_array(
                       std::meta::nonstatic_data_members_of(
                           ^^T, std::meta::access_context::current())))
      {
        std::string const key(std::string_view(reflex::serde::serialized_name(member)));
        properties.emplace(key, schema_of<typename[:std::meta::type_of(member):]>(defs));
        if constexpr(not reflex::serde::omits_when_empty(member))
        {
          required.emplace_back(key);
        }
      }
      defs[std::string(name)] = json::value{
          {"type",                 "object"             },
          {"additionalProperties", false                },
          {"properties",           std::move(properties)},
          {"required",             std::move(required)  }
      };
    }
    return {
        {"$ref", std::format("#/$defs/{}", name)}
    };
  }
}
}

auto parse_description(std::string_view json, std::string_view source) -> mcu_description
{
  mcu_description description;
  try
  {
    description = json::deserializer{json}.load<mcu_description>();
  }
  catch(std::exception const& failure)
  {
    refuse_other_version(json, source);
    throw config_error(
        std::string(source), 0, "",
        std::format("an MCU description matching the schema: {}", failure.what()));
  }
  if(description.schema_version != description_schema_version)
  {
    refuse_other_version(json, source);
    throw_version_missing(source);
  }
  return description;
}

auto load_description(std::filesystem::path const& file) -> mcu_description
{
  std::ifstream input(file, std::ios::binary);
  if(not input)
  {
    throw config_error(file.string(), 0, "", "a readable MCU description file");
  }
  std::ostringstream text;
  text << input.rdbuf();
  return parse_description(text.view(), file.string());
}

auto dump_description(mcu_description const& description) -> std::string
{
  auto stamped           = description;
  stamped.schema_version = description_schema_version;
  std::string      out;
  json::serializer serializer{out};
  serializer.dump(stamped);
  return out;
}

auto description_json_schema() -> std::string
{
  json::object defs;
  schema_of<mcu_description>(defs);
  auto root =
      std::move(defs.extract(std::string(std::meta::identifier_of(^^mcu_description))).mapped());
  root.as_object()["properties"].as_object()["schema_version"] = json::value{
      {"const", static_cast<json::number>(description_schema_version)}
  };
  root.as_object().emplace("$schema", "https://json-schema.org/draft/2020-12/schema");
  root.as_object().emplace("title", "popcorn MCU description");
  root.as_object().emplace("$defs", std::move(defs));

  std::string      out;
  json::serializer serializer{out};
  serializer.dump(root);
  return out;
}

auto description_digest(mcu_description const& description) -> std::string
{
  auto const digest = reflex::sha256_of(dump_description(description));
  auto const hex    = reflex::to_hex(digest);
  return std::string{hex.begin(), hex.end()};
}

auto flat_register_id(mcu_description const& description, register_ref ref) -> std::uint32_t
{
  auto const before =
      description.peripherals
      | std::views::take(ref.peripheral)
      | std::views::transform([](peripheral_desc const& p) { return p.registers.size(); });
  return static_cast<std::uint32_t>(std::ranges::fold_left(before, std::size_t{0}, std::plus{}))
       + ref.reg;
}

auto register_at(mcu_description const& description, std::uint32_t id)
    -> std::optional<register_ref>
{
  std::size_t remaining = id;
  for(auto const& [index, peripheral] : std::views::enumerate(description.peripherals))
  {
    if(remaining < peripheral.registers.size())
    {
      return register_ref{static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(remaining)};
    }
    remaining -= peripheral.registers.size();
  }
  return std::nullopt;
}
}
