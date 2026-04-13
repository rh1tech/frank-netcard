# FRANK Netcard

AT modem firmware for ESP-01 (ESP8266). The host MCU (e.g. RP2350 on [FRANK](https://rh1.tech/projects/frank?area=about)) sends AT commands over UART to manage WiFi and TCP/TLS/UDP sockets.

## Features

- AT commands over UART (115200 baud default, up to 3M)
- WiFi scanning, connect/disconnect, auto-reconnect
- 4 concurrent sockets -- TCP, TLS/SSL (BearSSL), UDP
- DNS resolution
- Binary data framing with async receive events
- Fits on ESP-01 (1MB flash, ~50KB free RAM)

## Hardware

- ESP-01 module (ESP8266, 1MB flash)
- USB-serial adapter for flashing and testing (3.3V FTDI or similar)
- Host MCU with UART (e.g. RP2350)

### Wiring

| ESP-01 Pin | Host MCU |
|------------|----------|
| TX         | UART RX  |
| RX         | UART TX  |
| VCC        | 3.3V     |
| GND        | GND      |
| CH_PD (EN) | 3.3V     |

No hardware flow control -- the ESP-01 does not expose CTS/RTS.

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE)

### Build

```bash
./build.sh
```

Or directly:

```bash
cd esp01-firmware
pio run
```

### Flashing

```bash
./flash.sh
```

Environment variables:
- `FLASH_PORT` -- serial port (auto-detected if not set)
- `FLASH_BAUD` -- flash baud rate (default 115200)

### Testing

```bash
# Basic tests (no WiFi needed)
./test.sh

# With WiFi
./test.sh --wifi MyNetwork,mypassword

# Full test (includes TLS + UDP)
./test.sh --wifi MyNetwork,mypassword --full

# Custom TLS host
./test.sh --wifi MyNetwork,mypassword --full --tls-host rh1.tech:443
```

## AT command protocol

UART at 115200 baud, 8N1 (default).

### Conventions

| Term | Meaning |
|------|---------|
| `\r\n` | Carriage return + line feed. Every command and response line ends with this. |
| `>` | Prompt. The modem is waiting for raw binary bytes (see [Data Send Flow](#atssendidalength)). |
| `OK` | Command succeeded. |
| `ERROR:<reason>` | Command failed. Reason is a short machine-readable token. |
| `+TAG:...` | Prefixed response data or async event. |

- Commands are ASCII, terminated by `\r\n` (bare `\r` or `\n` also accepted).
- Commands are case-insensitive (`at+wstat` = `AT+WSTAT`).
- The modem does **not** echo input.
- Unsolicited (async) events may arrive at any time while in command mode.

### Boot

On power-up or reset, the modem emits:

```
+READY\r\n
```

The host must wait for `+READY` before sending commands.

### System commands

#### AT

Test the connection.

```
-> AT
<- OK
```

#### AT+RST

Reset the module. Responds `OK`, then reboots. Wait for `+READY`.

```
-> AT+RST
<- OK
   ... module reboots ...
<- +READY
```

#### AT+VER

Query firmware version.

```
-> AT+VER
<- +VER:frank-netcard,1.0.0
<- OK
```

#### AT+HEAP

Query free heap memory (bytes). Useful before opening TLS connections.

```
-> AT+HEAP
<- +HEAP:38240
<- OK
```

#### AT+BAUD=\<rate\>

Change UART baud rate. Takes effect immediately after `OK`.
Valid range: 9600 -- 3000000.

```
-> AT+BAUD=230400
<- OK
   ... host must now switch to 230400 ...
```

### WiFi commands

#### AT+WSCAN

Scan for nearby access points. Blocks for a few seconds.

```
-> AT+WSCAN
<- +WSCAN:MyNetwork,-42,4,6
<- +WSCAN:OtherAP,-71,7,1
<- OK
```

Response fields: `ssid,rssi_dBm,encryption_type,channel`

Encryption types (ESP8266 constants):
| Value | Meaning |
|-------|---------|
| 2 | WPA (TKIP) |
| 4 | WPA2 (CCMP) |
| 5 | WEP |
| 7 | Open |
| 8 | WPA/WPA2 Auto |

#### AT+WJOIN=\<ssid\>,\<password\>

Connect to a WiFi network. Blocks up to 15 seconds.

```
-> AT+WJOIN=MyNetwork,mypassword123
<- +WJOIN:192.168.1.42,255.255.255.0,192.168.1.1
<- OK
```

Response fields: `ip,subnet_mask,gateway`

On failure:
```
<- ERROR:CONNECT_FAILED
```

#### AT+WQUIT

Disconnect from WiFi. Also closes all open sockets.

```
-> AT+WQUIT
<- OK
```

#### AT+WSTAT

Query current WiFi status.

When connected:
```
-> AT+WSTAT
<- +WSTAT:CONNECTED,MyNetwork,192.168.1.42,255.255.255.0,192.168.1.1,192.168.1.1,-42
<- OK
```

Response fields: `status,ssid,ip,subnet_mask,gateway,dns_server,rssi_dBm`

When disconnected:
```
-> AT+WSTAT
<- +WSTAT:DISCONNECTED
<- OK
```

### Network commands

#### AT+RESOLVE=\<hostname\>

Resolve a hostname via DNS. Requires active WiFi connection.

```
-> AT+RESOLVE=example.com
<- +RESOLVE:93.184.216.34
<- OK
```

On failure:
```
<- ERROR:DNS_FAILED
```

### Socket commands

Up to 4 concurrent sockets (IDs 0-3). TCP, TLS, and UDP.

#### AT+SOPEN=\<id\>,\<type\>,\<host\>,\<port\>

Open a socket connection.

- `id`: 0-3
- `type`: `TCP`, `TLS` (or `SSL`), `UDP`
- `host`: hostname or IP address (DNS resolution is automatic)
- `port`: 1-65535

```
-> AT+SOPEN=0,TCP,example.com,80
<- OK

-> AT+SOPEN=1,TLS,example.com,443
<- OK

-> AT+SOPEN=2,UDP,8.8.8.8,53
<- OK
```

Possible errors:
| Error | Meaning |
|-------|---------|
| `INVALID_ID` | ID not in 0-3 |
| `ALREADY_OPEN` | Socket ID already in use |
| `NO_WIFI` | Not connected to WiFi |
| `INVALID_PORT` | Port out of range |
| `LOW_MEMORY` | Not enough heap for TLS (need ~25KB free) |
| `DNS_FAILED` | Could not resolve hostname (TCP and TLS only) |
| `CONNECT_FAILED` | TCP connection or TLS handshake failed |
| `CONNECT_FAILED:ssl=N` | TLS handshake failed; `N` is the BearSSL error code |
| `BIND_FAILED` | Could not bind UDP local port |
| `INVALID_TYPE` | Unknown socket type |

TLS notes:
- Certificate verification is disabled (ESP-01 has too little RAM for a CA store).
- TLS uses BearSSL. One connection needs ~20-40KB of heap.
- In practice you get 1-2 concurrent TLS connections on an ESP-01.

#### AT+SSEND=\<id\>,\<length\>

Send binary data over a socket. Max length: 1024 bytes.

Flow:
```
-> AT+SSEND=0,13
<- >
-> Hello, World!
<- SEND OK
```

Step by step:
1. Host sends `AT+SSEND=0,13\r\n`
2. Modem responds `>\r\n` -- it is now in binary receive mode
3. Host sends exactly 13 raw bytes (no `\r\n` terminator needed)
4. Modem sends data over the socket, responds `SEND OK\r\n` or `SEND FAIL\r\n`

Watch out:
- After the `>` prompt, send exactly `length` raw bytes. No framing.
- Async events (+SRECV) are paused while in binary mode.
- 10-second timeout -- if no data arrives, the modem responds `ERROR:SEND_TIMEOUT` and returns to command mode.
- For payloads larger than 1024 bytes, split across multiple AT+SSEND commands.

#### AT+SCLOSE=\<id\>

Close a socket.

```
-> AT+SCLOSE=0
<- OK
```

#### AT+SSTAT

Query status of all open sockets.

```
-> AT+SSTAT
<- +SSTAT:0,TCP,CONNECTED,example.com,80
<- +SSTAT:1,TLS,CONNECTED,example.com,443
<- OK
```

Response fields: `id,type,state,remote_host,remote_port`

If no sockets are open, only `OK` is returned.

### Async events

These can arrive at any time while in command mode.
The host parser must handle them interleaved with command responses.

#### +SRECV:\<id\>,\<length\>\r\n\<data\>

Incoming socket data. After the `\r\n` that ends the header line,
exactly `length` raw bytes follow. No trailing `\r\n` after the data.

```
<- +SRECV:0,45\r\n<exactly 45 bytes of raw data>
```

Parsing on the host side:
1. Read a line (up to `\r\n`)
2. If line starts with `+SRECV:`, extract `id` and `length`
3. Read exactly `length` bytes -- these are raw payload (may contain `\r\n`)
4. Return to line-reading mode

Data arrives in chunks of up to 1024 bytes. A large response (e.g. HTTP body)
will produce multiple +SRECV events.

#### +SCLOSED:\<id\>

The remote end closed a TCP/TLS connection (or WiFi dropped).
All buffered data has already been delivered via +SRECV before this event fires.

```
<- +SCLOSED:0
```

#### +WDISCONN

WiFi connection lost. All sockets are automatically closed.
Each open socket also gets a +SCLOSED event.

```
<- +WDISCONN
<- +SCLOSED:0
<- +SCLOSED:1
```

#### +WCONN:\<ip\>

WiFi reconnected (auto-reconnect). Sockets are NOT restored -- they must be
reopened by the host.

```
<- +WCONN:192.168.1.42
```

### Example: HTTP GET

```
-> AT
<- OK

-> AT+WJOIN=MyWiFi,password123
<- +WJOIN:192.168.1.42,255.255.255.0,192.168.1.1
<- OK

-> AT+SOPEN=0,TCP,example.com,80
<- OK

-> AT+SSEND=0,37
<- >
-> GET / HTTP/1.0\r\nHost: example.com\r\n\r\n
<- SEND OK

<- +SRECV:0,512
<- <512 bytes: HTTP/1.0 200 OK...>
<- +SRECV:0,1024
<- <1024 bytes of HTML...>
<- +SRECV:0,89
<- <89 bytes: end of HTML>
<- +SCLOSED:0
```

### Example: HTTPS GET

```
-> AT+HEAP
<- +HEAP:42000
<- OK

-> AT+SOPEN=0,TLS,example.com,443
<- OK

-> AT+SSEND=0,39
<- >
-> GET / HTTP/1.0\r\nHost: example.com\r\n\r\n
<- SEND OK

<- +SRECV:0,1024
<- <response data...>
<- +SCLOSED:0
```

### Host driver notes

#### Parser architecture

The host UART parser is a line-oriented state machine:

```
STATE: READLINE
  |
  Read chars until \r\n -> got a line
  |
  +-- starts with "+SRECV:" -> extract id, length -> STATE: READDATA(length)
  +-- starts with "+SCLOSED:" / "+WDISCONN" / "+WCONN:" -> handle async event
  +-- starts with ">" -> currently in SSEND flow, send the data bytes
  +-- "OK" -> command complete (success)
  +-- "ERROR:..." -> command complete (failure)
  +-- "SEND OK" / "SEND FAIL" -> send complete
  +-- starts with "+" -> response data line (e.g. +WSTAT:..., +WSCAN:...)

STATE: READDATA(remaining)
  |
  Read raw bytes, decrement remaining
  |
  When remaining == 0 -> deliver data to application -> STATE: READLINE
```

#### Concurrency

Only one command at a time. Send a command, wait for `OK` or `ERROR` before sending the next. Async events (+SRECV, +SCLOSED, +WDISCONN) can arrive while waiting -- buffer them or dispatch immediately in your read loop.

#### Flow control

The ESP-01 does not expose CTS/RTS pins, so there is no hardware flow control.
At 115200 baud this is not a problem. For higher throughput:

- Increase baud with AT+BAUD (up to 3M)
- Keep host UART RX reads fast (interrupt-driven, not polled)
- The modem pauses async events during AT+SSEND binary mode

#### Reconnection

When +WDISCONN fires:
1. All sockets are already closed (you'll get +SCLOSED for each)
2. Wait for +WCONN (auto-reconnect is enabled)
3. Reopen any sockets you need

#### Memory

The ESP-01 has ~50KB free RAM at runtime. Rough costs:
- Each TCP socket: ~2-4KB
- Each TLS socket: ~20-40KB (BearSSL)
- Practical maximum: 4 TCP, or 1-2 TLS + 1-2 TCP
- Check AT+HEAP before opening TLS connections

## License

GNU General Public License v3. See [LICENSE](LICENSE) for details.

## Author

Mikhail Matveev <xtreme@rh1.tech>
