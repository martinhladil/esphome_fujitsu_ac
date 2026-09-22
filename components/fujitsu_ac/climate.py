"""Climate platform for the Fujitsu AC UART component."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

from . import FujitsuAC

DEPENDENCIES = ["uart"]
# Auto-load the base components used by this hub's entities (temperature
# sensors, plus the optional switch / select platforms).
AUTO_LOAD = ["sensor", "switch", "select"]

CONF_OUTDOOR_TEMPERATURE = "outdoor_temperature"
CONF_INDOOR_TEMPERATURE = "indoor_temperature"


def _temperature_sensor_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    )


CONFIG_SCHEMA = (
    climate.climate_schema(FujitsuAC)
    .extend(
        {
            cv.Optional(CONF_OUTDOOR_TEMPERATURE): _temperature_sensor_schema(),
            cv.Optional(CONF_INDOOR_TEMPERATURE): _temperature_sensor_schema(),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "fujitsu_ac",
    baud_rate=9600,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    await uart.register_uart_device(var, config)

    if CONF_OUTDOOR_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_OUTDOOR_TEMPERATURE])
        cg.add(var.set_outdoor_temperature_sensor(sens))
    if CONF_INDOOR_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_INDOOR_TEMPERATURE])
        cg.add(var.set_indoor_temperature_sensor(sens))
