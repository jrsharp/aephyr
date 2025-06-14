# AT Command Interface for Aether Network

This document defines the AT command interface for connecting retrocomputers to the Aether mesh network via a simple UART connection. This interface allows vintage computers with minimal resources to participate in the network while accessing the same services available to more powerful clients.

## Design Philosophy

The AT command interface follows these principles:
1. **Simplicity**: Commands follow the familiar Hayes AT command syntax
2. **Resource Efficiency**: Designed for computers with limited RAM and processing power
3. **Service Parity**: Access to the same services as modern clients
4. **Progressive Enhancement**: Basic functions work on all systems, advanced features optional

## Hardware Interface

```
┌───────────────────┐    UART     ┌───────────────────┐      ┌─────────────┐
│  Retrocomputer    │<----------->│ AT-Command LoRa   │<---->│ Aether Mesh │
│  (Apple II, etc.) │  9600 baud  │ Interface Device  │      │  Network    │
└───────────────────┘             └───────────────────┘      └─────────────┘
```

- **Connection**: RS-232 or TTL UART 
- **Default Baud Rate**: 9600 baud (configurable)
- **Flow Control**: Hardware (RTS/CTS) or XON/XOFF
- **Data Format**: 8-N-1 (8 data bits, no parity, 1 stop bit)

## Command Format

All commands follow standard AT command format:
- Commands begin with "AT"
- Command parameters are separated by commas
- Command ends with carriage return (`\r`)
- Responses end with `\r\n`

## Response Format

The interface uses these standard response formats:

```
OK\r\n                        # Command executed successfully
+<COMMAND>: <data>\r\n        # Data response
ERROR <code>\r\n              # Command failed with error code
>                             # Prompt for data input
```

Unsolicited notifications use this format:
```
+<EVENT>: <data>\r\n          # Notification of event
```

## Command Categories

### 1. System Commands

| Command | Description | Parameters | Example |
|---------|-------------|------------|---------|
| `AT` | Check if device is responsive | None | `AT` |
| `AT+RESET` | Reset the interface device | None | `AT+RESET` |
| `AT+VER?` | Query firmware version | None | `AT+VER?` |
| `AT+ECHO=<mode>` | Enable/disable command echo | 0=off, 1=on | `AT+ECHO=1` |
| `AT+VERBOSE=<level>` | Set verbosity level | 0-2 | `AT+VERBOSE=1` |
| `AT+BAUD=<rate>` | Set baud rate | Rate in bps | `AT+BAUD=19200` |
| `AT+FLOW=<mode>` | Set flow control | 0=none, 1=XON/XOFF, 2=RTS/CTS | `AT+FLOW=1` |
| `AT+SAVE` | Save current settings as default | None | `AT+SAVE` |
| `AT+HELP` | List available commands | None | `AT+HELP` |
| `AT+HELP=<cmd>` | Get help for specific command | Command name | `AT+HELP=MSGSEND` |

### 2. Network Commands

| Command | Description | Parameters | Example |
|---------|-------------|------------|---------|
| `AT+INIT` | Initialize LoRa hardware | None | `AT+INIT` |
| `AT+NETALIAS=<name>` | Set node alias name | Name (max 15 chars) | `AT+NETALIAS=APPLE2` |
| `AT+NETADDR?` | Query LoRa network address | None | `AT+NETADDR?` |
| `AT+NETID=<id>` | Set network ID | 4-byte hex | `AT+NETID=AEAE0001` |
| `AT+NETJOIN` | Join Aether network | None | `AT+NETJOIN` |
| `AT+NETLEAVE` | Leave Aether network | None | `AT+NETLEAVE` |
| `AT+NETSTAT` | Network status | None | `AT+NETSTAT` |
| `AT+NEIGH` | List neighbor nodes | None | `AT+NEIGH` |
| `AT+ROUTE?` | Query routing table | None | `AT+ROUTE?` |
| `AT+DUTYCYCLE?` | Query remaining duty cycle | None | `AT+DUTYCYCLE?` |
| `AT+SLEEP=<mode>` | Enter power saving mode | 0=disable, 1=light, 2=deep | `AT+SLEEP=1` |
| `AT+WAKE` | Wake from sleep mode | None | `AT+WAKE` |

### 3. Message Commands

| Command | Description | Parameters | Example |
|---------|-------------|------------|---------|
| `AT+MSGSEND=<dest>,<len>` | Begin message send | Destination, length | `AT+MSGSEND=BROADCAST,14` |
| `AT+MSGTO=<dest>` | Set default message destination | Node address/alias | `AT+MSGTO=NODE42` |
| `AT+MSGMODE=<mode>` | Set message mode | 0=text, 1=hex, 2=binary | `AT+MSGMODE=0` |
| `AT+MSGRECV` | Check for received messages | None | `AT+MSGRECV` |
| `AT+MSGREAD=<id>` | Read specific message | Message ID | `AT+MSGREAD=5` |
| `AT+MSGLIST` | List stored messages | None | `AT+MSGLIST` |
| `AT+MSGDEL=<id>` | Delete message | Message ID | `AT+MSGDEL=3` |
| `AT+MSGACK=<mode>` | Configure delivery reports | 0=disable, 1=enable | `AT+MSGACK=1` |
| `AT+MSGSENT?` | Check if message was sent | None | `AT+MSGSENT?` |

### 4. 9P/Styx Protocol Commands

| Command | Description | Parameters | Example |
|---------|-------------|------------|---------|
| `AT+9PATTACH=<addr>,<path>` | Attach to a 9P server | Address, path | `AT+9PATTACH=NODE42,/` |
| `AT+9PWALK=<fid>,<path>` | Walk to path | FID, path | `AT+9PWALK=3,/repo/wiki` |
| `AT+9POPEN=<fid>,<mode>` | Open file | FID, mode (r/w/rw) | `AT+9POPEN=5,r` |
| `AT+9PREAD=<fid>,<off>,<count>` | Read from file | FID, offset, count | `AT+9PREAD=5,0,100` |
| `AT+9PWRITE=<fid>,<off>,<len>` | Write to file | FID, offset, length | `AT+9PWRITE=7,0,10` |
| `AT+9PSTAT=<fid>` | Get stat information | FID | `AT+9PSTAT=5` |
| `AT+9PCLUNK=<fid>` | Close file | FID | `AT+9PCLUNK=5` |
| `AT+9PLS=<path>` | List directory contents | Path | `AT+9PLS=/repo` |
| `AT+9PCAT=<path>` | Display file contents | Path | `AT+9PCAT=/repo/README` |
| `AT+9PMKDIR=<path>` | Create directory | Path | `AT+9PMKDIR=/home/user1` |
| `AT+9PTOUCH=<path>` | Create empty file | Path | `AT+9PTOUCH=/home/user1/note.txt` |

### 5. Store-and-Forward Commands

| Command | Description | Parameters | Example |
|---------|-------------|------------|---------|
| `AT+SFQUEUE=<path>,<prio>` | Queue resource for transfer | Path, priority | `AT+SFQUEUE=/doc/manual.txt,1` |
| `AT+SFSTATUS=<id>` | Check status of queued resource | Queue ID | `AT+SFSTATUS=7` |
| `AT+SFLIST` | List queued transfers | None | `AT+SFLIST` |
| `AT+SFCACHE=<path>` | Cache resource locally | Path | `AT+SFCACHE=/repo/wiki/intro` |
| `AT+SFPURGE=<path>` | Remove from cache | Path | `AT+SFPURGE=/temp/old.txt` |
| `AT+SFPREFS=<type>,<val>` | Set S&F preferences | Type, value | `AT+SFPREFS=ttl,3600` |

### 6. Application Services Commands

| Command | Description | Parameters | Example |
|---------|-------------|------------|---------|
| `AT+SVCLIST` | List available services | None | `AT+SVCLIST` |
| `AT+SVCCONN=<name>` | Connect to service | Service name | `AT+SVCCONN=BBS` |
| `AT+SVCDISC=<name>` | Disconnect from service | Service name | `AT+SVCDISC=BBS` |
| `AT+BBSLS` | List BBS boards | None | `AT+BBSLS` |
| `AT+BBSREAD=<board>,<id>` | Read BBS post | Board, post ID | `AT+BBSREAD=general,5` |
| `AT+BBSPOST=<board>,<subj>,<len>` | Post to BBS | Board, subject, length | `AT+BBSPOST=general,Hello,140` |
| `AT+CHATJOIN=<room>` | Join chat room | Room name | `AT+CHATJOIN=lobby` |
| `AT+CHATSEND=<room>,<len>` | Send chat message | Room, length | `AT+CHATSEND=lobby,20` |
| `AT+FINGER=<user>` | Finger user information | Username | `AT+FINGER=bob` |

### 7. Security and Authentication

| Command | Description | Parameters | Example |
|---------|-------------|------------|---------|
| `AT+AUTH=<user>,<pass>` | Authenticate with credentials | Username, password | `AT+AUTH=bob,secret` |
| `AT+AUTHKEY=<type>,<key>` | Configure authentication key | Type, key data | `AT+AUTHKEY=psk,01A2B3...` |
| `AT+ENCRYPT=<mode>` | Set encryption mode | 0=off, 1=basic, 2=full | `AT+ENCRYPT=1` |

## Sample Interaction Sequences

### Sending a Message

```
Computer: AT+INIT
Device:   OK

Computer: AT+NETJOIN
Device:   OK
Device:   +NETJOIN: CONNECTED

Computer: AT+MSGSEND=BROADCAST,14
Device:   >                   # Device ready for data
Computer: Hello, Aether!      # Send data bytes
Device:   OK
Device:   +MSGSEND: 1         # Message ID assigned
```

### Reading a File from Wiki

```
Computer: AT+9PATTACH=NODE42,/repo/wiki
Device:   OK
Device:   +9PATTACH: 5        # FID assigned

Computer: AT+9PREAD=5,0,100
Device:   +9PREAD: 78         # Bytes returned
Device:   Welcome to the community wiki! This resource contains...
Device:   OK
```

### Posting to BBS

```
Computer: AT+SVCCONN=BBS
Device:   OK
Device:   +SVCCONN: CONNECTED

Computer: AT+BBSPOST=general,Hello from Apple II,120
Device:   >
Computer: This is my first post from my vintage Apple II computer via the Aether network. Amazing to be connecting a 40+ year old machine to a LoRa mesh!
Device:   OK
Device:   +BBSPOST: 42        # Post ID assigned
```

## Error Codes

| Code | Description |
|------|-------------|
| 0 | Unknown error |
| 1 | Invalid command |
| 2 | Invalid parameter |
| 3 | Network not initialized |
| 4 | Network not connected |
| 5 | Transmission failed |
| 6 | Message too long |
| 7 | No message available |
| 8 | Invalid FID |
| 9 | File not found |
| 10 | Permission denied |
| 11 | Insufficient resources |
| 12 | Duty cycle limit reached |
| 13 | Service unavailable |
| 14 | Authentication failed |

## Implementation Considerations

### Memory Management

- **Limited Buffers**: Interface device handles buffering to accommodate retrocomputer limitations
- **Progressive Transfer**: Large content automatically chunked into manageable pieces
- **Memory-Mapped I/O**: Options for direct memory access where supported

### Performance Optimizations

- **Command Shortcuts**: Common operations have abbreviated forms
- **State Persistence**: Device maintains state to reduce command overhead
- **Local Caching**: Frequently accessed resources cached on interface device

### Retrocomputer-Specific Adaptations

- **ASCII Translation**: Automatic translation between different character sets
- **Terminal Emulation**: Basic ANSI/VT100 emulation for formatted content
- **Screen-Oriented Modes**: Special modes for common screen dimensions (40x24, 80x25)

## Implementation Examples

### Apple II BASIC Example

```basic
10 PRINT CHR$(4);"PR#2"       : REM OPEN COMM SLOT
20 PRINT "AT+INIT"            : REM INITIALIZE
30 INPUT A$                   : REM WAIT FOR OK
40 PRINT "AT+NETJOIN"         : REM JOIN NETWORK
50 INPUT B$                   : REM WAIT FOR OK
60 PRINT "AT+9PATTACH=MAIN,/" : REM ATTACH TO SERVER
70 INPUT C$                   : REM GET RESULT
80 PRINT "AT+9PLS=/"          : REM LIST ROOT DIR
90 INPUT D$                   : REM GET FIRST LINE
100 PRINT D$                  : REM DISPLAY RESULT
110 IF LEFT$(D$,1) = "+" THEN GOTO 90 : REM MORE DATA
120 PRINT CHR$(4);"PR#0"      : REM CLOSE COMM
```

### Commodore 64 Example

```basic
10 OPEN 2,2,0,"":REM OPEN COMM DEVICE
20 PRINT#2,"AT+INIT"
30 INPUT#2,A$:PRINT A$
40 PRINT#2,"AT+NETJOIN"
50 INPUT#2,B$:PRINT B$
60 PRINT#2,"AT+BBSLS"
70 INPUT#2,C$:PRINT C$
80 IF LEFT$(C$,1)="+" THEN 70
90 CLOSE 2
```

## Future Extensions

Planned future extensions include:
- File transfer protocols adapted for retrocomputers (XMODEM/YMODEM)
- Graphical content adaptation for limited display capabilities
- Direct memory access modes for efficient transfers
- Specific commands for common retrocomputer peripherals

## References

1. Hayes AT Command Set Standard
2. Plan 9 9P/Styx Protocol Documentation
3. Aether Mesh Network Architecture
4. Store-and-Forward Architecture Documentation 