"""Fujitsu AC UART climate component (UTY-TFSXW1 protocol)."""

import esphome.codegen as cg
from esphome.components import climate, uart

CODEOWNERS = ["@martinhladil"]

fujitsu_ac_ns = cg.esphome_ns.namespace("fujitsu_ac")

FujitsuAC = fujitsu_ac_ns.class_(
    "FujitsuAC", climate.Climate, uart.UARTDevice, cg.Component
)

# Used by the switch / select platforms to reference the hub.
CONF_FUJITSU_AC_ID = "fujitsu_ac_id"
