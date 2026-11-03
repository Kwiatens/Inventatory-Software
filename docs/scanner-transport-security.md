# Scanner transport

Here I describe how the Inventatory Hardware <-> Software communication works.

> Current version of the protocol: v1

## Pairing process

Over Bluetooth the desktop sends a random 32-byte secret to the scanner. On Windows it keeps that information in the WCM (Windows Credential Manager), on Linux in the LSS (Linux Secret Service).
The device keeps its secret in NVS (non-volatile memory).

Pairing persists only on one workspace - if system is moved on a new desktop, then pairing process has to be redone.

## Finding the PC

Scanner looks up `_inventatory._tcp` over mDNS. This only finds the PC, it proves nothing about it.

## Requests

The scanner sends `POST /api/v1/device/sync` for sync, and `POST /api/v1/device/firmware/offer`, `chunk` and `status` for updates.

Every request carries:

| Header | Value |
| --- | --- |
| `X-Inventatory-Protocol` | `1` |
| `X-Inventatory-Device` | the device id |
| `X-Inventatory-Counter` | a number that only goes up |
| `X-Inventatory-Mac` | HMAC-SHA-256, hex |

The response carries the same counter and its own MAC.

## Keys and MACs

Each direction has its own key that's derived from the secret:

```text
request key  = HMAC-SHA-256(secret, "Inventatory Scanner R1 transport v1 client-to-server")
response key = HMAC-SHA-256(secret, "Inventatory Scanner R1 transport v1 server-to-client")
```

The MACs cover these exact strings:

```text
request:  "Inventatory Scanner R1/v1\nrequest\n<method>\n<path>\n<device>\n<counter>\n<body>"
response: "Inventatory Scanner R1/v1\nresponse\n<counter>\n<status>\n<body>"
```

So a request MAC can never pass as a response MAC. Changing any byte of the body breaks it.

## Replay

The PC stores the highest counter it accepted inside of `inventatory-scanner-replay.state` in the workspace.
It refuses any counter that is lower or equal, also after a restart.

The scanner saves the next counter in NVS before it sends, so a retry always uses a new number.

## Limits

Request bodies are <= 64 KiB.

Response bodies <= 8 KiB.

## Firmware updates

The desktop offers a `firmware.ota` package, that was signed with a key it trusts.
The scanner then checks the signature and asks the user for confirmation.

## Reset

Regenerating the secret and/or clearing the device makes a new secret, resets the replay state and forgets
the device.

## What it does not do

There is **NO** encryption of the data packets that move around from the scanner to the desktop as for now.
Anyone on the local network can read the JSON messages made from the scanning process, but cannot forge a message, replay one, or recover the secret.
