# Cyber Sled's link: the C139 ring

Cyber Sled links cabinets for head-to-head play through the board's C139
serial chip. MAME only maps the chip (16 KB of RAM, a status register that
always reads 4): no link is emulated. What is below was read from the slave
68000's program (`cy2-spr-u/l`), which does all of the link's work; the
addresses are the slave's.

## The chip, as the slave uses it

C139 RAM at `B00000h-B03FFFh` (8K words), registers at `B80000h`:

| Register | Use |
|---|---|
| `B80000h` | status: bit 1 a packet received, bit 2 a packet sent; written 0 to clear |
| `B80002h` | interrupt enables (the handler leaves `9`, `B` or `D` in it) |
| `B80006h` | 1 while sending, 0 when the sending is over |
| `B8000Ah` | the packet's length in words (`5Eh`): writing it sends |
| `B8000Ch` | read: the word last received in the receive ring |
| `B8000Eh` | the packet to send: its first word's address in the RAM, in words |

At start (`3692h`) the slave writes a register setup table (`3672h`) to
`B80000h`, clears the RAM, writes a second table, and marks every node slot
empty.

Its interrupt is **level 4** (vector at `37D8h`), the C148's serial level.

## The RAM

| Words | Use |
|---|---|
| `10h` (`B00020h`) | the round trip of this cabinet's own packet (written by the handler) |
| `11h-12h` (`B00022h-24h`) | which nodes were heard in the last 16 frames (a mask, high/low byte) |
| `20h-21h` (`B00040h-42h`) | which nodes' slots to send (a mask): this cabinet's own, and the others' it relays |
| `800h + n*80h` | node n's slot (n 0-15): the packet last sent or relayed for node n |
| `1000h-1FFFh` | the receive ring: packets as they arrive, one after another, wrapping |

## A packet

`5Eh` (94) words, each a byte in the low half:

| Word | |
|---|---|
| 0 | the node it comes from (0-15) |
| 1 | `99h`, a mark |
| 2 | hops: 1 when sent, one more at each cabinet that relays it; dropped at 16 |
| 3-92 | the data: 90 bytes (the slot's own state: sled, shots...) |
| 93 | the checksum (the sum of the bytes before it) with bit 8 set: the packet's end |

## What the slave does

**Each frame** (`3700h`, from the level 5 handler at `1CFAh`, and again from
`1704h`):

- every 16 frames, the mask of nodes heard goes to `B00022h/24h`;
- for each node in the send mask, the node's slot gets its header (node,
  `99h`, hops 1), its bytes and its checksum, and is queued;
- if anything is queued, the serial interrupt is enabled.

**The interrupt** (`37D8h`):

- **sent** (status bit 2): the next queued slot goes out (`B8000Eh` its
  address, `B80006h` 1, `B8000Ah` `5Eh`);
- **received** (status bit 1): from `B8000Ch`, back through the ring to the
  word with bit 8 set, then 94 words back: the packet. It checks the mark,
  adds a hop (more than 15: dropped) and the checksum. A packet of its own
  (back after going round the ring) only records its round trip. Another
  node's is copied into that node's slot (from word 3), counted, and queued
  to be relayed: the ring passes every packet on to the next cabinet.

**This cabinet's node number** is bit 0 of the NVRAM's byte `12h` (the main
68000's `180024h`, read at boot after the I/O MCU's handshake, `1186h`):
cabinet 0 or 1, so a link is two cabinets. The main 68000 passes it to the
slave (shared RAM `900228h`); with it, the slave builds the send mask
(`3188h`) and counts the nodes heard (`900100h` lists them).

## To link two cabinets

The game does all the protocol itself; what is missing is the chip:

- a send: the packet's 94 words, from the RAM at `B8000Eh`, go to the next
  cabinet; some time later (the wire's), status bit 2 and the interrupt;
- a receive: a packet from the previous cabinet is written into the ring
  after the last one, `B8000Ch` at its last word, status bit 1 and the
  interrupt;
- the ring: with two cabinets, each one's next is the other. Over a local
  network: UDP, a datagram a packet.

Each cabinet needs its own node number (0 and 1: NVRAM byte `12h`), and the two
must run in step: the game sends every frame and expects the others' packets
within a few frames.

## In the port

- `src/c139.cpp`: the chip, as the slave expects it. Status bit 2 is "ready
  to send" (the chip idle), bit 1 "received" (cleared by a write); the
  interrupt mask's bits (1 = masked) are set by the chip as it interrupts
  and cleared by the game to re-arm, as its handler's ANDIs imply. A send
  takes about 8000 cycles. Packets wait their turn: the next goes into the
  ring once the game has taken the last.
- `src/link.cpp`: the network. Each cabinet broadcasts
  `S21L1 H <set> <id> <port>` every second on UDP port 47121 (and to its
  own computer, so two copies on one PC link); two running the same game
  pair up, the lower id cabinet 1 (node 0). A packet is a datagram to the
  other's port; three seconds without one: unpaired.
- The CONTROLS page's LINK row (a game with `linkNvram` in its profile):
  OFF, or ON with how it stands (SEARCHING, CABINET 1, CABINET 2). Kept in
  `save/options.ini`. Paired, the cabinet's number goes into the NVRAM
  (byte 12h, the settings' checksum in 16h) and, if it changed, the game
  restarts (its boot hidden as ever).
- Off, the chip answers as before (status 4, no interrupt): a lone
  cabinet plays as it always did.

Checked with two copies on one PC: they pair in about a second, each game
hears the other (the slave's mask of nodes heard: `10D14Eh`) and keeps
hearing it; about three packets a frame each way (its own and the other's,
relayed back round the ring).

The packets are taken eight times a frame (the visible part run in eighths),
not once at vblank: the arcade's boards heard each other at once, and with a
frame's delay the cabinet that had not chosen the course could still act on
its own (a button pressed while it waited took it to a vehicle select of its
own, and the two never met again). Reproduced with two copies on one PC, a
script pressing fire on the waiting one; with the packets taken eight times
a frame, the two meet in the match every time.
