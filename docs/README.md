[README.md](https://github.com/user-attachments/files/22266143/README.md)
# keep_it_cold

Multi-node ESP32 LoRa DS18B20 temperature monitoring network  
by @tscofield

## Features

- DS18B20 temperature sensor (accurate, waterproof)
- LoRa peer-to-peer sync: node list, temperature, alarms
- OLED display for local status
- Configurable via web interface (NodeID, WiFi, node management, time, alarm silence)
- REST API for integration
- Node-down and check-in alarm logic (with daytime buzzer only)
- Manual timekeeping: no Internet required!

## Hardware Requirements

- ESP32 Dev Board
- SX1262 LoRa module (HiLetgo or similar)
- DS18B20 temperature sensor
- SSD1306 OLED display (I2C, 128x64)
- Passive buzzer
- Breadboard or PCB

## Wiring Diagram

See [`docs/wiring.svg`](docs/wiring.svg)

### Pin Connections (default, see `main.cpp`)
| ESP32 Pin | Device     | Function      |
|-----------|------------|---------------|
| 17        | DS18B20    | Data          |
| 21        | OLED SDA   | I2C Data      |
| 22        | OLED SCL   | I2C Clock     |
| 32        | Buzzer     | Alarm         |
| 5,19,27,18,14,26 | LoRa | SPI + Control |

## Setup

1. Flash each node with `src/main.cpp`.
2. Wire per the diagram.
3. Power up, connect to WiFi (default SSID: `ESP32Probe`, PASS: NodeID).
4. Access web UI at `http://<device-ip>/`.
5. Configure NodeID, WiFi, node list, time, alarms.
6. Add all expected NodeIDs to each node (via web UI, automatically syncs).
7. Each node broadcasts its temperature and listens for peers.

## Web Interface

- Set NodeID, WiFi SSID/Password
- Add nodes to the peer list (syncs to all)
- Silence alarms for 1 hour
- Set system time (no Internet required)
- View current and peer temperatures
- REST API: `/api/temps` for JSON data

## Alarms

- Node-down: If any peer fails to send heartbeat for 30s, alarm triggers.
- Check-in: If nobody has used the web UI in 24 hours, alarm triggers.
- Only buzzes during 8:00–20:00.
- All alarms can be silenced via web UI.

## Timekeeping

- Set time manually via web UI or serial.
- ESP32 keeps time internally (millis + stored epoch).
- All alarm logic uses this local time.

## Example Serial Commands

- `SETNODEID:ABCDEF` — Set NodeID
- `SETWIFI:myssid,mywifipass` — Set WiFi
- `SETTIME:2025,09,11,14,00` — Set time (YYYY,MM,DD,HH,mm)

## License

MIT
