# XMRig simple-mode Stratum protocol

The server implements the CryptoNote `simple` protocol used by ordinary XMRig
for Monero `rx/0`. It is plain TCP: one UTF-8 JSON object per LF-terminated
line. TLS and proxy-protocol identity are not built in.

## Framing and request envelope

- LF terminates a frame; a single CR immediately before LF is removed.
- The configured line limit excludes LF.
- Empty frames, NUL, invalid JSON, duplicate object keys, excessive JSON depth,
  non-object roots, missing/invalid IDs, and overlong input are protocol
  violations. Gross framing/malformed JSON closes the connection.
- `id` is required and is either a signed 64-bit JSON integer or a UTF-8 string
  of 1..128 bytes. Its type and value are echoed exactly.
- A `(type,value)` request ID may not identify two live operations on one
  connection. It becomes reusable after the earlier complete response frame is
  atomically accepted into the bounded output queue.
- `jsonrpc` may be omitted for compatibility; when present it is exactly
  `"2.0"`.
- `method` is a string and `params` is an object. Miner notifications without
  IDs are invalid.
- Supported methods are exactly `login`, `submit`, and `keepalived`. Before
  authentication only `login` is allowed.

Output is bounded per connection. If a complete result/job cannot fit, the
connection closes and latest-sent height does not advance. A login must arrive
before `login_timeout_ms`; authenticated activity must remain within
`idle_timeout_ms`.

## Login

```json
{
  "id": 1,
  "jsonrpc": "2.0",
  "method": "login",
  "params": {
    "login": "rig-01",
    "pass": "x",
    "agent": "XMRig/6.26.0",
    "rigid": "rack-a",
    "algo": ["rx/0"]
  }
}
```

Allowed parameter keys are only `login`, `pass`, `agent`, `rigid`, and `algo`.
`login` and `pass` are required strings. Login is nonempty and at most 256
bytes; rigid is at most 256 and agent at most 512. Rigid/agent default to empty.
NUL is rejected. Login is only an accounting label—not a payout address—and
duplicates are allowed.

`algo`, if present, is string `rx/0` or a nonempty unique string array that
contains `rx/0`. NiceHash, self-select, wallet-routing, and other extension
parameters are rejected.

Authentication is exact and untrimmed:

```text
configured null/empty -> any string pass accepted
configured nonempty   -> constant-time exact match required
```

In `minimum` difficulty mode, a final `+<positive uint64 decimal>` is removed
from the logical login and can raise (never lower) the configured floor. Bad or
overflowing suffix syntax rejects login. In fixed mode `+...` is ordinary label
text.

Successful response:

```json
{
  "id": 1,
  "jsonrpc": "2.0",
  "error": null,
  "result": {
    "id": "0123456789abcdef0123456789abcdef",
    "job": {
      "blob": "<hashing-blob-hex>",
      "job_id": "<32-lowercase-hex>",
      "target": "<16-lowercase-hex>",
      "algo": "rx/0",
      "height": 3736190,
      "seed_hash": "<64-lowercase-hex>"
    },
    "extensions": ["algo", "keepalive"],
    "status": "OK"
  }
}
```

The result `id` is a private 16-byte connection RPC ID. It is required in
later submits/keepalives and is unrelated to the request ID.

## New jobs

Each installed valid daemon template derives a new private job for every
authenticated connection. It is sent as a notification:

```json
{
  "jsonrpc": "2.0",
  "method": "job",
  "params": {
    "blob": "<hashing-blob-hex>",
    "job_id": "<32-lowercase-hex>",
    "target": "<16-lowercase-hex>",
    "algo": "rx/0",
    "height": 3736191,
    "seed_hash": "<64-lowercase-hex>"
  }
}
```

The server retains the configured current/prior job history and applies every
nonce to the exact retained private block. `connection_last_sent_height` is
the height of the latest complete job frame successfully queued, not a
maximum-ever height. A downward reorg can lower it.

## Difficulty target

For assigned difficulty `D >= 1`:

```text
target64 = floor((2^64 - 1) / D)
```

The job `target` is that unsigned value serialized as exactly eight
little-endian bytes and then 16 lowercase hex characters. A raw RandomX hash
passes the share target only when the little-endian `uint64` in raw
`hash[24..31]` is strictly less than `target64`; equality is low difficulty.

Network-candidate detection is independent and uses the complete 256-bit raw
little-endian hash with daemon unsigned 128-bit difficulty `D`:

```text
H * D <= 2^256 - 1
```

The 64-bit shortcut must not be used as a block test.

## Submit

```json
{
  "id": 2,
  "jsonrpc": "2.0",
  "method": "submit",
  "params": {
    "id": "0123456789abcdef0123456789abcdef",
    "job_id": "<32-hex-private-job-id>",
    "nonce": "d0030040",
    "result": "e1364b8782719d7683e2ccd3d8f724bc59dfa780a9e960e7c0e0046acdb40100",
    "algo": "rx/0"
  }
}
```

Allowed keys are only `id`, `job_id`, `nonce`, `result`, and optional `algo`.
The connection ID must match; job ID decodes to exactly 16 bytes and belongs to
this connection; nonce is exactly four raw bytes/eight hex; result is exactly
32 raw bytes/64 hex; algo, if present, is `rx/0`. Hex is case-insensitive at
decode and normalized internally. Nonce bytes are copied as submitted—there is
no host-endian numeric reinterpretation.

A monotonically increasing per-connection `request_sequence` is assigned after
strict parameter/length decoding. Request-ID reuse after a prior response
creates a distinct share row; the request ID is correlation text, not a
database uniqueness key.

Success:

```json
{
  "id": 2,
  "jsonrpc": "2.0",
  "error": null,
  "result": {"status": "OK"}
}
```

Application errors use code `-1` and one of the intentionally short messages
`Unauthenticated`, `Unknown job`, `Duplicate share`, `Low difficulty share`,
`Stale share`, `Invalid result`, or `Server busy`. Syntax/envelope and unknown
method errors use standard `-32600`/`-32601` where a response can safely be
formed. Rich internal codes remain in SQLite/API/events.

The response is only the ordinary share classification. It is never a promise
that a candidate block was accepted. Candidate submit/retry/reconciliation and
`blocknotify` continue independently, and no second Stratum response reports
their outcome.

Verified mode responds after the MSPV completion and durable final share
transaction. Trusted mode responds after synchronous claimed-hash
classification. If the TCP connection disappears first, the result remains
persisted and no response is sent.

## Classification order and staleness

After structural checks, the runtime reserves
`private_entropy || claimed_hash`, freezes a possible claimed candidate, and
submits the exact hashing blob to MSPV when enabled. Final ordinary-share
precedence is:

```text
verifier infrastructure failure -> Server busy / no credit
authoritative duplicate          -> Duplicate share
claimed/computed mismatch        -> Invalid result
missed assigned target           -> Low difficulty share
valid older work                 -> Stale share
valid current unique work        -> OK and assigned-difficulty credit
```

Stale is true only for otherwise valid work when the latest job successfully
queued to this same connection has a strictly higher height. Same-height older
jobs are not stale. Global daemon height, another miner, a template fetch, or a
failed job enqueue is irrelevant. Unknown/expired work is `Unknown job`, not
stale.

A structurally valid claimed network candidate may be durably journaled and
sent immediately even when its height is older; `monerod` is the block
authority. Its ordinary share response still follows validation above.

## Keepalive

```json
{"id":3,"method":"keepalived","params":{"id":"0123456789abcdef0123456789abcdef"}}
```

Only the exact `id` key is allowed and it must match the connection. A valid
request resets idle activity and receives:

```json
{
  "id": 3,
  "jsonrpc": "2.0",
  "error": null,
  "result": {"status": "KEEPALIVED"}
}
```

## XMRig configuration

```json
{
  "autosave": false,
  "pools": [
    {
      "url": "127.0.0.1:3333",
      "user": "rig-01",
      "pass": "x",
      "algo": "rx/0",
      "keepalive": true,
      "enabled": true
    }
  ]
}
```
