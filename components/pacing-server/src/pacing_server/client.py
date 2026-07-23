from __future__ import annotations

from typing import Any

import zmq

from market_data_protocol import control_message, decode_message, encode_message


def control_request(endpoint: str, command: str, **parameters: Any) -> dict[str, Any]:
    context = zmq.Context.instance()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.LINGER, 0)
    socket.connect(endpoint)
    try:
        socket.send(
            encode_message(control_message("CONTROL", command=command, **parameters))
        )
        return decode_message(socket.recv())["body"]
    finally:
        socket.close(linger=0)
