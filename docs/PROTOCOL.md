# Multiplayer Racing Server Protocol Specification (v1.0)

This document defines the wire protocols used for real-time multiplayer racing communication between the C++20 authoritative server and browser clients.

---

## 1. Protocol Architecture & Lifecycle

Communication utilizes standard **RFC 6455 WebSocket** connections established over the HTTP server port (default: `8080`).

```
Browser Client                                   C++20 Server (epoll Loop)
      |                                                      |
      | -------- GET /ws HTTP/1.1 (Upgrade: websocket) ----> |
      | <------- HTTP/1.1 101 Switching Protocols ---------- |
      |                                                      |
      | <======= Welcome Frame {"type":"welcome"} ========== |
      |                                                      |
      | -------- Client Input / Room Control Frame --------> |
      |                                                      |
      | <======= 20 Hz World Snapshots (Binary or JSON) ==== |
      |                                                      |
      | -------- Close Frame (1000 Normal Closure) --------> |
      | <------- Close Frame Echo -------------------------- |
```

### 1.1 Connection Handshake

Clients initiate connection with a standard HTTP GET request to `/ws`:
```http
GET /ws?format=binary HTTP/1.1
Host: racing.local:8080
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
Sec-WebSocket-Protocol: racing-binary-v1
```

Server response:
```http
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
Sec-WebSocket-Protocol: racing-binary-v1
```

### 1.2 Format Negotiation

The server supports two snapshot formats:
1. **Binary Snapshots (Default & Recommended)**: High-performance, zero-allocation binary protocol yielding over **72% bandwidth savings**.
2. **JSON Snapshots (Fallback)**: Human-readable JSON protocol for debugging and legacy clients.

**Negotiation Methods:**
- **URL Query Parameter**: `/ws?format=binary` or `/ws?format=json`.
- **WebSocket Subprotocol**: Header `Sec-WebSocket-Protocol: racing-binary-v1`.
- **Runtime Format Switching**: Clients can switch format dynamically at any time by sending a text control frame:
  ```json
  {"type": "set_format", "format": "binary"}
  ```

---

## 2. Binary Snapshot Wire Specification (Schema v1)

Binary snapshots are transmitted as unfragmented **WebSocket Binary Frames** (`opcode = 0x02`). All multi-byte integer and floating-point values are encoded in **Network Byte Order (Big-Endian)**.

### 2.1 Header Layout (36 Bytes Fixed)

The frame starts with a fixed 36-byte header:

| Byte Offset | Field Name | Data Type | Endianness | Description |
|:---:|:---|:---:|:---:|:---|
| `0–3` | `magic` | `uint32` | Big-Endian | Magic number: `0x534E4150` (ASCII `"SNAP"`) |
| `4` | `version` | `uint8` | — | Protocol schema version: `1` |
| `5` | `roomState` | `uint8` | — | Room State: `0`=WAITING, `1`=COUNTDOWN, `2`=RACING, `3`=FINISHED |
| `6–7` | `countdown` | `uint16` | Big-Endian | Countdown in tenths of a second (e.g. `50` = 5.0s, `0` = inactive) |
| `8–11` | `roomId` | `uint32` | Big-Endian | Room ID (`0` for Practice Mode, `>0` for Multi-Room) |
| `12–19` | `roomCode` | `char[8]` | ASCII | Fixed 8-byte ASCII room code (null-padded / null-terminated) |
| `20–23` | `tick` | `uint32` | Big-Endian | Low 32 bits of monotonic 60 Hz simulation tick counter |
| `24–31` | `timestamp` | `uint64` | Big-Endian | Server epoch timestamp in milliseconds |
| `32–33` | `playerCount` | `uint16` | Big-Endian | Number of player vehicle records following header |
| `34–35` | `resultCount` | `uint16` | Big-Endian | Number of finish leaderboard records following players |

### 2.2 Per-Player Record Layout (36 Bytes Fixed Per Player)

Following the 36-byte header, exactly `playerCount` records of 36 bytes each are packed consecutively:

| Byte Offset | Field Name | Data Type | Endianness | Description |
|:---:|:---|:---:|:---:|:---|
| `+0–3` | `id` | `uint32` | Big-Endian | Authoritative Server Player ID |
| `+4–7` | `x` | `float32` | Big-Endian | Normalized lateral road position in `[-1.0, 1.0]` (IEEE 754) |
| `+8–11` | `distance` | `float32` | Big-Endian | Longitudinal spline progress in meters (IEEE 754) |
| `+12–15` | `speed` | `float32` | Big-Endian | Vehicle forward speed in km/h (IEEE 754) |
| `+16–19` | `steer` | `float32` | Big-Endian | Steering angle in radians (IEEE 754) |
| `+20–23` | `worldX` | `float32` | Big-Endian | Pre-calculated 2D Cartesian world X coordinate (IEEE 754) |
| `+24–27` | `worldY` | `float32` | Big-Endian | Pre-calculated 2D Cartesian world Y coordinate (IEEE 754) |
| `+28–31` | `heading` | `float32` | Big-Endian | Vehicle orientation heading angle in radians (IEEE 754) |
| `+32–35` | `ackSeq` | `uint32` | Big-Endian | Highest client input sequence number processed by server |

Total size for `N` players = `36 + (N * 36)` bytes.

### 2.3 Per-Result Record Layout (11 Bytes Fixed Header + Variable Name)

When a race is completed (`resultCount > 0`), finish records are appended after all player records:

| Byte Offset | Field Name | Data Type | Endianness | Description |
|:---:|:---|:---:|:---:|:---|
| `+0–3` | `playerId` | `uint32` | Big-Endian | Finished player ID |
| `+4–5` | `rank` | `uint16` | Big-Endian | Final finishing rank (`1` = 1st place) |
| `+6–9` | `finishTime` | `float32` | Big-Endian | Official finishing time in seconds (IEEE 754) |
| `+10` | `nameLen` | `uint8` | — | Byte length `M` of player display name (`0..255`) |
| `+11–(10+M)` | `name` | `char[M]` | UTF-8 | UTF-8 encoded player display name string (`M` bytes) |

---

## 3. JSON Control & Lifecycle Protocol

Text frames (`opcode = 0x01`) carry UTF-8 encoded JSON objects for lobby management, room creation, and client input.

### 3.1 Server Welcome Frame
Dispatched immediately when client establishes connection:
```json
{
  "type": "welcome",
  "playerId": 1,
  "tick": 1205,
  "timestamp": 20.0833
}
```

### 3.2 Client Input Frame (60 Hz Transmission)
Transmitted by the client during active gameplay:
```json
{
  "type": "input",
  "seq": 142,
  "controls": {
    "up": true,
    "down": false,
    "left": false,
    "right": false
  }
}
```

### 3.3 Room Management Messages

#### Create Room
**Client Request:**
```json
{
  "type": "create_room",
  "name": "SpeedRacer",
  "trackLength": 1500.0
}
```
**Server Response:**
```json
{
  "type": "room_created",
  "roomId": 1,
  "roomCode": "ABC123",
  "playerId": 1,
  "hostId": 1,
  "room": {
    "id": 1,
    "code": "ABC123",
    "hostPlayerId": 1,
    "state": "WAITING",
    "capacity": 8,
    "playerCount": 1,
    "trackLength": 1500.0,
    "countdownSeconds": 5.0,
    "players": [
      {
        "id": 1,
        "name": "SpeedRacer",
        "ready": false,
        "lane": 0
      }
    ],
    "results": []
  }
}
```

#### Join Room
**Client Request:**
```json
{
  "type": "join_room",
  "code": "ABC123",
  "name": "Challenger"
}
```
**Server Broadcast to Room Members:**
```json
{
  "type": "player_joined",
  "roomId": 1,
  "playerId": 2,
  "name": "Challenger"
}
```

#### Toggle Ready Status
```json
{
  "type": "set_ready",
  "ready": true
}
```

#### Start Race (Host Only)
```json
{
  "type": "start_race"
}
```
**Authoritative Countdown Broadcast:**
```json
{
  "type": "countdown",
  "roomId": 1,
  "countdownSeconds": 5.0
}
```

#### Leave Room
```json
{
  "type": "leave_room"
}
```

---

## 4. Error Codes & Handling

When an operation fails, the server sends a `room_error` frame:
```json
{
  "type": "room_error",
  "message": "Room not found or code invalid"
}
```

### WebSocket Close Codes
- `1000 Normal Closure`: Clean client or server shutdown.
- `1002 Protocol Error`: Non-RFC 6455 framing, unmasked client frame, reserved opcode.
- `1007 Invalid Payload Data`: Malformed UTF-8 text in text frame.
- `1008 Policy Violation`: Connection buffer or control frame flooding limits exceeded.
- `1009 Message Too Big`: Frame or message exceeded maximum permitted payload size.
- `1013 Try Again Later`: Server lifecycle queue or room capacity at maximum threshold.
