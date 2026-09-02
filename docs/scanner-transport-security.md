# Inventatory Scan R1 transport v1

The R1 connects to the PC after discovering `_inventatory._tcp` over mDNS.
mDNS is only discovery: it does not authenticate the advertised host.

During the existing physically verified Bluetooth LE pairing flow, the PC
provisions a random 32-byte pairing secret. It is retained by the Windows
Credential Manager on the PC and NVS on the R1. The secret is never placed in
an HTTP request or response.

Each `POST /api/v1/device/sync` carries the device id, a monotonic request
counter, and an HMAC-SHA-256 MAC over the method, path, device id, counter, and
exact JSON body. The PC stores the highest accepted counter under its local
application settings and refuses lower or equal counters, including after
restart. The R1 reserves the next counter in NVS before sending, so a failed
attempt can be retried only with a newer counter.

The PC signs each response with a distinct, derived server-to-client key and
the same request counter. The R1 rejects unsigned, altered, mismatched, or
replayed responses. Derived directional keys prevent a request MAC from being
accepted as a response MAC.

This protocol provides device binding, integrity, replay resistance, and
mutual authentication against an active local-network attacker. It does not
provide payload confidentiality: request and response JSON can still be read
by a local-network observer. An observer cannot recover or replay the pairing
secret from that traffic. The R1-compatible bridge continues to bind on the LAN
interface rather than loopback. On Windows, Inventatory installs an app-owned
inbound firewall rule for the current executable and port, limited to the
Private profile and `LocalSubnet`. This narrows the trust boundary to the
private LAN, but it is not encryption; users should treat that network as
trusted and avoid exposing the bridge through public profiles or port
forwarding. If Windows denies the rule, the bridge remains usable and the
terminal keeps a persistent warning so the user can fix the firewall manually.
TLS with a device trust/bootstrap model remains a future enhancement.

Regenerate-secret and clear-device actions rotate the pairing secret, reset
replay state, clear the paired device identity, and require a fresh pairing.
