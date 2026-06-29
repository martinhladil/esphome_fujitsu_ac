"""On/off feature registers (§9.3 / §10.5) exposed as switches."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_FUJITSU_AC_ID, FujitsuAC, fujitsu_ac_ns

FujitsuACSwitch = fujitsu_ac_ns.class_("FujitsuACSwitch", switch.Switch)

# config key -> (hub setter, icon). The register address for each feature lives
# in the C++ hub (protocol.h), assigned when the entity is registered.
SWITCH_TYPES = {
    "coil_dry": ("set_coil_dry_switch", "mdi:water-percent"),
    "outdoor_low_noise": ("set_outdoor_low_noise_switch", "mdi:volume-low"),
    "minimum_heat": ("set_minimum_heat_switch", "mdi:snowflake-thermometer"),
    "energy_saving_fan": ("set_energy_saving_fan_switch", "mdi:fan-auto"),
    "human_sensor": ("set_human_sensor_switch", "mdi:motion-sensor"),
}

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_FUJITSU_AC_ID): cv.use_id(FujitsuAC)}
).extend(
    {
        cv.Optional(key): switch.switch_schema(
            FujitsuACSwitch,
            icon=icon,
            entity_category=ENTITY_CATEGORY_CONFIG,
            # The switch mirrors the unit; never command it from a saved state at boot.
            default_restore_mode="DISABLED",
        )
        for key, (_setter, icon) in SWITCH_TYPES.items()
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_AC_ID])
    for key, (setter, _icon) in SWITCH_TYPES.items():
        if key not in config:
            continue
        var = await switch.new_switch(config[key])
        await cg.register_parented(var, parent)
        cg.add(getattr(parent, setter)(var))
