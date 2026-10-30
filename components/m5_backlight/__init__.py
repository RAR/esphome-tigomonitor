import esphome.config_validation as cv
from esphome.components import esp32

CODEOWNERS = ["@mrgalaxy"]
DEPENDENCIES = ["esp32"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    # Pulls in the real M5Unified/M5GFX library via the ESP Component
    # Registry, exactly like tigo_monitor's own cjson dependency. This is
    # the same library/code path proven (via a standalone test firmware)
    # to correctly drive the AtomS3R's LP5562 backlight and ST7735S/GC9107
    # panel — ESPHome's own i2c: component could not reproduce that result
    # despite matching every register/value/order/frequency M5's own code
    # uses, so this calls the real thing directly instead of re-deriving it.
    esp32.add_idf_component(name="m5stack/m5unified", ref="^0.2.28")
