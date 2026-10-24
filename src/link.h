// link.h: two cabinets linked over the local network, for a game whose
// boards linked through the C139 ring (Cyber Sled: docs/link.md). Each
// announces itself by UDP broadcast; two running the same game pair up, and
// the game's packets go between them as datagrams. The cabinet numbers (0
// and 1) come from comparing the two cabinets' random ids: both agree.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class Link {
public:
    Link();
    ~Link();

    // Start looking for another cabinet running this game (MAME's set name).
    // False if the network cannot be used (the reason in error()).
    bool open(const char *set);
    void close();
    bool isOpen() const { return open_; }
    const std::string &error() const { return error_; }

    // Once a frame: announce this cabinet, read what has arrived.
    void poll();

    bool paired() const { return paired_; }
    int cabinet() const { return cabinet_; }       // 0 or 1, once paired

    // A packet of the game's (words) to the other cabinet; dropped unpaired.
    void send(const uint16_t *words, int count);
    // The next packet from the other cabinet, oldest first.
    bool receive(std::vector<uint16_t> &words);

private:
    void announce(bool toPeerToo);
    void handle(const uint8_t *data, int size, uint32_t fromAddr);
    double now() const;

    bool open_ = false;
    std::string set_, error_;
    uint64_t id_ = 0;
    intptr_t hello_ = -1, data_ = -1;      // the sockets: the shared announce port, this cabinet's own
    int dataPort_ = 0;

    bool paired_ = false;
    int cabinet_ = 0;
    uint64_t peerId_ = 0;
    uint32_t peerAddr_ = 0;                // IPv4, network order
    int peerPort_ = 0;
    double lastHeard_ = 0, lastAnnounce_ = -1;
    std::deque<std::vector<uint16_t>> in_;
};
