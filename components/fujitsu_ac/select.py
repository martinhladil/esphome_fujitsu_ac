"""Vertical / horizontal airflow position (§10.4) exposed as selects."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_FUJITSU_AC_ID, FujitsuAC, fujitsu_ac_ns

FujitsuACVaneSelect = fujitsu_ac_ns.class_("FujitsuACVaneSelect", select.Select)

# Must match VANE_OPTIONS in fujitsu_ac.cpp (§10.4 position codes 0x01–0x06 + Swing).
# "Closed" (0x0000) is the reported-only state shown while the unit is off.
VANE_OPTIONS = [
    "Position 1",
    "Position 2",
    "Position 3",
    "Position 4",
    "Position 5",
    "Position 6",
    "Swing",
    "Closed",
]

# config key -> (hub setter, icon). The position/swing register pair for each
# axis lives in the C++ hub (protocol.h), assigned when the entity is registered.
SELECT_TYPES = {
    "vertical_vane": ("set_vertical_vane_select", "mdi:arrow-up-down"),
    "horizontal_vane": ("set_horizontal_vane_select", "mdi:arrow-left-right"),
}

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_FUJITSU_AC_ID): cv.use_id(FujitsuAC)}
).extend(
    {
        cv.Optional(key): select.select_schema(
            FujitsuACVaneSelect,
            icon=icon,
            entity_category=ENTITY_CATEGORY_CONFIG,
        )
        for key, (_setter, icon) in SELECT_TYPES.items()
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_AC_ID])
    for key, (setter, _icon) in SELECT_TYPES.items():
        if key not in config:
            continue
        var = await select.new_select(config[key], options=VANE_OPTIONS)
        await cg.register_parented(var, parent)
        cg.add(getattr(parent, setter)(var))
