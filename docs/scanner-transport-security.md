# Inventatory Scan R1 transport v1

The R1 connects to the PC after discovering `_inventatory._tcp` over mDNS.
mDNS is only discovery: it does not authenticate the advertised host.

During the existing physically verified Bluetooth LE pairing flow, the PC
provisions a random 32-byte pairing secret. It is retained by the Windows
Credential Manager on the PC and NVS on the R1. The secret is never placed in
an HTTP request or response.

Each `POST /api/v1/device/sync` carries the device id, a monotonic request
counter, and an HMAC-SHA-256 MAC over the method, path, device id, counter, and
exact JSON body. The PC stores the highest accepted counter in the selected
workspace's `inventatory-scan-replay.state` file and refuses lower or equal
counters, including after restart. The R1 reserves the next counter in NVS
before sending, so a failed attempt can be retried only with a newer counter.

The PC signs each response with a distinct, derived server-to-client key and
the same request counter. The R1 rejects unsigned, altered, mismatched, or
replayed responses. Derived directional keys prevent a request MAC from being
accepted as a response MAC.

## Response-size contract

The authenticated JSON body of a successful `POST /api/v1/device/sync`
response is limited to 8,192 bytes. This is a protocol limit shared with the
R1 firmware, not merely a desktop HTTP-server setting. The firmware also caps
the response headers and rejects missing, malformed, truncated, trailing, or
chunked bodies before parsing or applying any result.

The desktop serializer bounds optional descriptive text at the protocol
boundary while preserving event identifiers, status, deltas, quantities, and
other state needed for synchronization. If a callback still produces a body
outside the limit, the desktop returns an authenticated `500` error with a
small retryable JSON status body; it does not send a partial response. The R1
keeps its outbox event because only a successful `200` response can acknowledge
it. An authenticated `5xx` also retains a pending quick-label print request;
the desktop caches that request result by ID, so retrying delivers the result
without issuing a duplicate print. Boundary tests cover realistic and
deliberately oversized inventory, lookup, quick-label, and print-result data.

This protocol provides device binding, integrity, replay resistance, and
mutual authentication against an active local-network attacker. It does not
provide payload confidentiality: request and response JSON can still be read
by a local-network observer. An observer cannot recover or replay the pairing
secret from that traffic. The R1-compatible bridge continues to bind on the LAN
interface rather than loopback. Inventatory does not change Windows Firewall
settings. If Scan R1 cannot connect, the user can manually allow the
executable's current service port on the Private network profile, limited to
the local network. This is a user-owned trust decision; the port should not be
exposed through public profiles or port forwarding. TLS with a device
trust/bootstrap model remains a future enhancement.

Regenerate-secret and clear-device actions rotate the pairing secret, reset
replay state, clear the paired device identity, and require a fresh pairing.

Pairing credentials are always scoped to the selected workspace. A newly
created or copied workspace does not inherit another workspace's credential or
replay state; if its pairing metadata has no matching scoped credential, the
Scan R1 service remains disabled until the user pairs that workspace again.
