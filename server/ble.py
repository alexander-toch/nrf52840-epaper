import sys
import logging
import aiohttp
import asyncio
import threading
import random

from typing import Any, Union

from bless import (  # type: ignore
    BlessServer,
    BlessGATTCharacteristic,
    GATTCharacteristicProperties,
    GATTAttributePermissions,
)

from secrets import HA_AUTH, HA_ENDPOINT

SERVER_NAME = "SPServer"  # must be shorter than 10 characters
SERVICE_UUID = "D2EA587F-19C8-4F4C-8179-3BA0BC150B01"
CHARACTERISTICS = [
    "0DF8D897-33FE-4AF4-9E7A-63D24664C94C",
    "0DF8D897-33FE-4AF4-9E7A-63D24664C94D",
    "0DF8D897-33FE-4AF4-9E7A-63D24664C94E",
    "0DF8D897-33FE-4AF4-9E7A-63D24664C94F",
]

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(name=__name__)

# NOTE: Some systems require different synchronization methods.
trigger: Union[asyncio.Event, threading.Event]
if sys.platform in ["darwin", "win32"]:
    trigger = threading.Event()
else:
    trigger = asyncio.Event()


def read_request(characteristic: BlessGATTCharacteristic, **kwargs) -> bytearray:
    logger.debug(f"Reading {characteristic.value}")
    return characteristic.value


async def get_ha_data():
    headers = {
        "Authorization": HA_AUTH,
        "Content-Type": "application/json",
    }
    async with aiohttp.ClientSession() as session:
        async with session.get(HA_ENDPOINT, headers=headers) as response:
            return await response.text()

server = None
async def run(loop):
    trigger.clear()

    # Instantiate the server
    server = BlessServer(name=SERVER_NAME, loop=loop)
    server.read_request_func = read_request

    await server.add_new_service(SERVICE_UUID)

    char_flags = GATTCharacteristicProperties.read | GATTCharacteristicProperties.notify
    permissions = GATTAttributePermissions.readable

    for uuid in CHARACTERISTICS:
        await server.add_new_characteristic(
            SERVICE_UUID, uuid, char_flags, None, permissions
        )

    await server.start()
    logger.debug("Advertising")

    while True and not trigger.is_set():
        logger.debug("Updating HA data")
        data = (await get_ha_data()).encode("utf-8")
        split_val = 240
        values = [data[i : i + split_val] for i in range(0, len(data), split_val)]
        for i, val in enumerate(values):
            server.get_characteristic(CHARACTERISTICS[i]).value = val
        logger.debug("Updated HA data")
        await asyncio.sleep(60)

    await server.stop()


loop = asyncio.get_event_loop()
loop.run_until_complete(run(loop))