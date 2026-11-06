#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/rcc.hpp>

#include <reflex/py.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::bind::detail
{
/** @brief Binds `I2CTargetBase`, `SpiTargetBase`, `SpiControllerBase`, `I2CControllerBase` and
 *        `UartDeviceBase`, the trampolines over the devices. */
void bind_device_bases(reflex::py::nb::module_ scope);

class callback_cell;

/** @brief Supplies `periph::stm32::board_inputs_of(description)`, called once the bus checks have
 *         passed. */
using device_inputs =
    std::function_ref<std::map<std::string, std::vector<std::string_view>, std::less<>> const&()>;

/** @brief Board members and bookkeeping of the Python devices, between `prepare_devices` and
 *         `device_factories`. */
struct device_plan
{
  /** @brief Peripherals that get a factory, with the RCC when the board sets its clock sources. */
  std::vector<std::string> names;
  /** @brief The callback cells of each peripheral's Python devices. */
  std::map<std::string, std::vector<std::shared_ptr<callback_cell>>, std::less<>> cells;
};

/**
 * @brief Checks the Python devices against @p inputs and puts them on @p board, with the RCC
 *        clock sources.
 * @param description The MCU.
 * @param devices Python devices per peripheral name, `popcorn.board.I2CTarget`, `SpiTarget`,
 *        `SpiController`, `I2CController` or `UartDevice` instances, an SPI target carrying its
 *        chip select as `_cs_port` and `_cs_pin`.
 * @param rcc Board clock sources of the RCC model, empty for its defaults and no RCC factory.
 * @param inputs Supplies `periph::stm32::board_inputs_of(description)`, called after the bus
 *        checks of every device.
 * @param board Receives the devices in `i2c`, `i2c_controllers`, `spi`, `spi_controllers` and
 *        `uarts`, and @p rcc in `rcc`.
 * @param python_owned Receives, borrowed, each Python device of @p devices whose reference a
 *        factory now holds, once per reference.
 * @return The peripherals that get a factory, sorted by name, and the cells to stop.
 * @throws config_error when a named peripheral is not an I2C, SPI or UART peripheral a built-in
 *         model serves, a target is of the other bus kind, an object is not a target, an
 *         SpiController, an I2CController or a UartDevice shares its instance with another device.
 *         A peripheral with no targets is skipped. With @p rcc, when no built-in model serves an
 *         RCC of @p description.
 */
[[nodiscard]] auto prepare_devices(
    part_description const&                                           description,
    std::map<std::string, std::vector<reflex::py::nb::object>> const& devices,
    std::optional<periph::stm32::rcc_options> const&                  rcc,
    device_inputs                                                     inputs,
    periph::stm32::board&                                             board,
    std::vector<reflex::py::nb::handle>&                              python_owned) -> device_plan;

/**
 * @brief Takes the factory of each peripheral of @p plan from @p all.
 * @param plan Result of `prepare_devices`.
 * @param all `periph::stm32::factories` of the board `prepare_devices` filled, consumed for the
 *        peripherals of @p plan.
 */
[[nodiscard]] auto device_factories(
    device_plan&                                               plan,
    std::map<std::string, periph::model_factory, std::less<>>& all)
    -> std::map<std::string, periph::model_factory, std::less<>>;
}
