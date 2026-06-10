# Tachyon configuration

Tachyon currently has no production configuration file. Runtime knobs are kept
small and explicit so the demo remains reproducible.

## C++ demos

`tachyon` is a deterministic scripted walkthrough and takes no arguments.

`tachyon_stream` emits newline-delimited JSON for dashboard or file consumers:

```sh
./build/bin/tachyon_stream [n_events=200] [depth=5] [delay_ms=0]
```

- `n_events`: number of events to emit; `0` means run until killed.
- `depth`: book levels per side included in each snapshot.
- `delay_ms`: sleep between events, useful for visual dashboard playback.

## Dashboard bridge

`dashboard/server.js` serves static files and bridges `tachyon_stream` stdout to
WebSocket clients. It reads these environment variables:

| variable | default | purpose |
| --- | --- | --- |
| `PORT` | `8080` | HTTP/WebSocket port. |
| `TACHYON_STREAM` | auto-detected under `build/bin` | Path to the stream binary. |
| `N_EVENTS` | `0` | Events to stream; `0` means continuous. |
| `DEPTH` | `8` | Snapshot depth sent to the UI. |
| `DELAY_MS` | `50` | Delay between generated events. |

Health checks:

- `GET /health`
- `GET /healthz`

Both return JSON with process uptime, stream binary path, connected client count,
and a request id.

Example:

```sh
PORT=8080 N_EVENTS=500 DELAY_MS=20 node dashboard/server.js
curl http://localhost:8080/health
```
