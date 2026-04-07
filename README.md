# esp32-cf-webserver

A minimal web server running on an ESP32, exposed to the internet through a Cloudflare Tunnel using [esp32-cf](https://github.com/melvinvoetberg/esp32-cf).

Serves a single status page showing live chip info, heap, uptime, and connection details — all rendered with a `{{server.*}}` template engine.

## Quick start

```bash
git clone https://github.com/melvinvoetberg/esp32-cf-webserver
cd esp32-cf-webserver
cp .env.example .env
# edit .env with your WiFi and tunnel credentials
source .env
pio run -e esp32-c6 -t upload
pio run -e esp32-c6 -t uploadfs
```

## Configuration

Set these environment variables before building (see `.env.example`):

| Variable | Required | Description |
|---|---|---|
| `WIFI_SSID` | yes | WiFi network name |
| `WIFI_PASS` | yes | WiFi password |
| `CF_TUNNEL_TOKEN` | no | From `cloudflared tunnel token <NAME>`. Leave empty for quick tunnel. |
| `CF_HOSTNAME` | no | Public hostname for named tunnel |
| `CF_CONNECTOR_ID` | no | Stable UUID (`uuidgen`). Persists connector identity across reboots. |

## Supported boards

- ESP32
- ESP32-S3
- ESP32-C3
- ESP32-C6

Change the board with `-e`:

Install the software:
```bash
pio run -e esp32-s3 -t upload
```

Upload data folder:
```bash
pio run -e esp32-s3 -t uploadfs
```

## Template tags

The HTML in `data/` supports these template variables:

| Tag | Example |
|---|---|
| `{{server.chip}}` | ESP32-C6 (rev 0.2) |
| `{{server.ip}}` | 192.168.1.105 |
| `{{server.hostname}}` | esp.example.com |
| `{{server.edge}}` | AMS |
| `{{server.heap}}` | 287432 |
| `{{server.uptime}}` | 3600 |
| `{{server.requests}}` | 42 |
| `{{server.wifi.ssid}}` | MyNetwork |
| `{{server.wifi.rssi}}` | -52 |

## License

MIT
