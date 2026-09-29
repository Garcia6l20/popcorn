#include "../register_layer.hpp"
#include "factories.hpp"

#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <array>
#include <mutex>
#include <string_view>

namespace popcorn::periph::stm32
{
namespace
{
struct builtin
{
  std::string_view ip;
  std::string_view ip_version_glob;
  auto (*make)(board const& b) -> model_factory;
  std::string_view instance_glob = "*";
};

constexpr std::array<builtin, 18> builtins{
    {
     {"RCC", "STM32U5_rcc_*", [](board const& b) { return rcc_factory(b.rcc); }},
     {"ICACHE", "Icache_v1_0_0*", [](board const& b) { return icache_factory(b.caches); }},
     {"DCACHE", "STM32U5xx1_v0_0", [](board const& b) { return dcache_factory(b.caches); }},
     {"PWR", "STM32U5_pwr_*", [](board const& b) { return pwr_factory(b.pwr); }},
     {"flash", "v1.0.0_U5_Cube", [](board const&) { return flash_factory(); }},
     {"GPIO", "STM32U5x_gpio_*", [](board const&) { return gpio_factory(); }, "GPIO?"},
     {"EXTI", "", [](board const&) { return exti_factory(); }},
     {"GPDMA", "STM32U5_dma3_*", [](board const&) { return gpdma_factory(); }},
     {"USART", "sci3_v2*", [](board const& b) { return usart_factory(b); }},
     {"UART", "sci2_*", [](board const& b) { return usart_factory(b); }},
     {"LPUART", "sci3_v1*", [](board const& b) { return usart_factory(b); }},
     {"TIM1_8U5", "gptimer2_v4_x*", [](board const&) { return tim_factory(); }},
     {"TIM6_7U5", "gptimer2_v4_x*", [](board const&) { return tim_factory(); }},
     {"IWDG", "iwdg1_v3*", [](board const&) { return iwdg_factory(); }},
     {"RTC", "rtc2_v2_U5*", [](board const& b) { return rtc_factory(b.rtc_initial); }},
     {"TAMP", "STM32L5_rtc2_v2_8*", [](board const&) { return tamp_factory(); }},
     {"SPI", "spi2s3_v1*", [](board const& b) { return spi_factory(b); }},
     {"I2C", "i2c2_v1_1U5*", [](board const& b) { return i2c_factory(b); }},
     }
};

auto default_board() -> board const&
{
  static board const instance{};
  return instance;
}
}

void register_builtin_models()
{
  static std::once_flag once;
  std::call_once(once, [] {
    for(auto const& entry : builtins)
    {
      register_model(entry.ip, entry.ip_version_glob, entry.make(default_board()));
    }
  });
}

auto factories(board const& b, mcu_description const& part)
    -> std::map<std::string, model_factory, std::less<>>
{
  std::map<std::string, model_factory, std::less<>> out;
  for(auto const& unit : part.peripherals)
  {
    if(unit.alias_of)
    {
      continue;
    }
    for(auto const& entry : builtins)
    {
      if(entry.ip
         == unit.ip
         and glob_match(entry.ip_version_glob, unit.ip_version)
         and glob_match(entry.instance_glob, unit.name))
      {
        out.emplace(unit.name, entry.make(b));
        break;
      }
    }
  }
  return out;
}
}
