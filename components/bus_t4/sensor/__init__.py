import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID

from .. import bus_t4_ns
from ..cover import BusT4Cover

DEPENDENCIES = ['bus_t4', 'cover']

CONF_COVER_ID = 'cover_id'
CONF_REGISTER = 'register'
CONF_MOTOR = 'motor'
CONF_BYTE = 'byte'
CONF_OFFSET = 'offset'
CONF_LENGTH = 'length'

BusT4RegisterSensor = bus_t4_ns.class_(
    'BusT4RegisterSensor', sensor.Sensor, cg.PollingComponent
)

CONFIG_SCHEMA = (
    sensor.sensor_schema(BusT4RegisterSensor, icon='mdi:register')
    .extend(cv.polling_component_schema('60s'))
    .extend(
        {
            # The bus_t4 cover owns the bus conversation: it sends the request
            # and receives the reply
            cv.Required(CONF_COVER_ID): cv.use_id(BusT4Cover),
            # Control unit parameter address, e.g. 0x4A (force, opening)
            cv.Required(CONF_REGISTER): cv.hex_uint8_t,
            # Motor/encoder index sent as the request argument. Needed by the
            # MC824H for its per-motor parameters; omit for parameters that are
            # not per motor.
            cv.Optional(CONF_MOTOR): cv.int_range(min=0, max=15),
            # Publish a single byte of the reply instead of the whole payload as
            # a big-endian integer. Several parameters have a layout that is not
            # established yet (0x4A/0x4B), so selecting a byte by position is the
            # only safe way to expose them. Equivalent to offset: N, length: 1.
            cv.Optional(CONF_BYTE): cv.int_range(min=0, max=15),
            # Skip this many bytes of the reply before decoding, for parameters
            # whose value is prefixed by another field: 0x24/0x25 answer
            # [motor index] [2-byte value], so offset: 1 skips the index.
            cv.Optional(CONF_OFFSET, default=0): cv.int_range(min=0, max=15),
            # Number of bytes to combine, big-endian. 0 (default) means "to the
            # end of the reply".
            cv.Optional(CONF_LENGTH, default=0): cv.int_range(min=0, max=4),
        }
    )
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_COVER_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_register(config[CONF_REGISTER]))
    cg.add(var.set_offset(config[CONF_OFFSET]))
    cg.add(var.set_decoded_length(config[CONF_LENGTH]))

    if CONF_MOTOR in config:
        cg.add(var.set_motor(config[CONF_MOTOR]))
    if CONF_BYTE in config:
        cg.add(var.set_byte(config[CONF_BYTE]))
