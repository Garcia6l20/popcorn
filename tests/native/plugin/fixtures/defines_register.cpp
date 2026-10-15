#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <string_view>

void popcorn::periph::register_model(std::string_view, std::string_view, model_factory)
{}

POPCORN_PLUGIN(defines_register, r)
{
  static_cast<void>(r);
}
