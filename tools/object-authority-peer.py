#!/usr/bin/env python3
"""Deterministic local host for exercising the in-game object-authority bridge."""

import argparse
import socket
import struct
import time


MAGIC = b"CCS\x01"
HELLO, WELCOME, HEARTBEAT, OBJECT_ACTION = 1, 2, 3, 5
PICKUP_REQUEST, PICKUP_GRANTED, PICKUP_DENIED, DROP = 1, 2, 3, 4
WIRE = struct.Struct("!4sBIQ")
ACTION = struct.Struct("!4sBBIIQII")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=24883)
    parser.add_argument("--pickup", choices=("grant", "deny"), default="grant")
    args = parser.parse_args()

    session = (int(time.monotonic() * 1000) & 0xFFFFFFFF) | 1
    leases: dict[int, tuple[int, int]] = {}
    responses: dict[int, bytes] = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    print(f"authority peer listening on 127.0.0.1:{args.port}; pickup={args.pickup}", flush=True)

    while True:
        packet, peer = sock.recvfrom(2048)
        if len(packet) == WIRE.size:
            magic, kind, packet_session, nonce = WIRE.unpack(packet)
            if magic != MAGIC:
                continue
            if kind == HELLO:
                sock.sendto(WIRE.pack(MAGIC, WELCOME, session, nonce), peer)
                print(f"guest connected from {peer[0]}:{peer[1]}", flush=True)
            elif kind == HEARTBEAT and packet_session == session:
                sock.sendto(packet, peer)
            continue

        if len(packet) != ACTION.size:
            continue
        magic, wire_type, kind, packet_session, request, object_id, player, epoch = ACTION.unpack(packet)
        if magic != MAGIC or wire_type != OBJECT_ACTION or packet_session != session:
            continue
        if request in responses:
            sock.sendto(responses[request], peer)
            continue

        holder, current_epoch = leases.get(object_id, (0, 1))
        response_kind = PICKUP_DENIED
        if kind == PICKUP_REQUEST and player == 2 and holder == 0 and args.pickup == "grant":
            holder = player
            current_epoch += 1
            response_kind = PICKUP_GRANTED
        elif kind == DROP and player == holder and epoch == current_epoch:
            holder = 0
            current_epoch += 1
            response_kind = DROP
        leases[object_id] = (holder, current_epoch)
        response = ACTION.pack(
            MAGIC, OBJECT_ACTION, response_kind, session, request, object_id, player, current_epoch
        )
        responses[request] = response
        sock.sendto(response, peer)
        label = {PICKUP_GRANTED: "granted", PICKUP_DENIED: "denied", DROP: "dropped"}[response_kind]
        print(f"request={request} object={object_id:016x} {label} epoch={current_epoch}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
