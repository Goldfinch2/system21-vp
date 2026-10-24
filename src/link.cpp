// link.cpp: the link's network side (link.h). Two UDP sockets: one on the
// shared announce port (several programs may listen on it, so two copies on
// one PC can link too) and one of this cabinet's own for the packets.
//
//   announce, broadcast and to this computer, every second:
//       "S21L1 H <set> <id, 16 hex> <packet port>"
//   a packet, to the other cabinet's packet port:
//       "S21L1 P" <id, 8 bytes> <the words, big-endian>
#include "link.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
static const Socket kNone = INVALID_SOCKET;
static void closeSocket(Socket s) { closesocket(s); }
static bool nonBlocking(Socket s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on) == 0; }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Socket;
static const Socket kNone = -1;
static void closeSocket(Socket s) { ::close(s); }
static bool nonBlocking(Socket s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
#endif

namespace {

const int ANNOUNCE_PORT = 47121;
const double ANNOUNCE_EVERY = 1.0;      // seconds
const double LOST_AFTER = 3.0;          // seconds without a word from the other cabinet: unpaired
const char MAGIC[] = "S21L1 ";
const int MAX_WORDS = 256;

Socket sock(intptr_t s) { return (Socket)s; }

} // namespace

Link::Link() = default;

Link::~Link() { close(); }

double Link::now() const
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool Link::open(const char *set)
{
    close();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error_ = "no network"; return false; }
#endif
    set_ = set;
    std::random_device rd;
    id_ = ((uint64_t)rd() << 32 ^ rd()) ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();

    Socket h = socket(AF_INET, SOCK_DGRAM, 0), d = socket(AF_INET, SOCK_DGRAM, 0);
    if (h == kNone || d == kNone) {
        if (h != kNone) closeSocket(h);
        if (d != kNone) closeSocket(d);
        error_ = "no network";
        return false;
    }
    int on = 1;
    setsockopt(h, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(h, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof on);
#endif
    setsockopt(h, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(ANNOUNCE_PORT);
    sockaddr_in b = a;
    b.sin_port = 0;
    socklen_t len = sizeof b;
    if (bind(h, (sockaddr *)&a, sizeof a) != 0 || bind(d, (sockaddr *)&b, sizeof b) != 0 ||
        getsockname(d, (sockaddr *)&b, &len) != 0 || !nonBlocking(h) || !nonBlocking(d)) {
        closeSocket(h);
        closeSocket(d);
        error_ = "network port busy";
        return false;
    }
    hello_ = (intptr_t)h;
    data_ = (intptr_t)d;
    dataPort_ = ntohs(b.sin_port);
    open_ = true;
    paired_ = false;
    lastAnnounce_ = -1;
    in_.clear();
    error_.clear();
    return true;
}

void Link::close()
{
    if (!open_) return;
    closeSocket(sock(hello_));
    closeSocket(sock(data_));
    hello_ = data_ = -1;
    open_ = false;
    paired_ = false;
    in_.clear();
#ifdef _WIN32
    WSACleanup();
#endif
}

// "S21L1 H <set> <id> <port>": to everyone on the network and to this
// computer (a second copy of the program), and to the other cabinet.
void Link::announce(bool toPeerToo)
{
    char msg[160];
    const int n = snprintf(msg, sizeof msg, "%sH %s %016llx %d", MAGIC, set_.c_str(), (unsigned long long)id_, dataPort_);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(ANNOUNCE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(sock(hello_), msg, n, 0, (sockaddr *)&a, sizeof a);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(sock(hello_), msg, n, 0, (sockaddr *)&a, sizeof a);
    if (toPeerToo && peerAddr_) {
        a.sin_addr.s_addr = peerAddr_;
        sendto(sock(hello_), msg, n, 0, (sockaddr *)&a, sizeof a);
    }
}

void Link::poll()
{
    if (!open_) return;
    const double t = now();
    if (lastAnnounce_ < 0 || t - lastAnnounce_ >= ANNOUNCE_EVERY) {
        announce(true);
        lastAnnounce_ = t;
    }
    uint8_t buf[2048];
    for (Socket s : { sock(hello_), sock(data_) }) {
        for (;;) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const int n = (int)recvfrom(s, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &len);
            if (n <= 0) break;
            handle(buf, n, from.sin_addr.s_addr);
        }
    }
    if (paired_ && t - lastHeard_ > LOST_AFTER) {
        paired_ = false;
        in_.clear();
    }
}

void Link::handle(const uint8_t *data, int size, uint32_t fromAddr)
{
    const int m = (int)sizeof MAGIC - 1;
    if (size < m + 1 || memcmp(data, MAGIC, m) != 0) return;
    if (data[m] == 'H') {
        char set[64];
        unsigned long long id = 0;
        int port = 0;
        std::string text((const char *)data + m, (size_t)(size - m));
        if (sscanf(text.c_str(), "H %63s %llx %d", set, &id, &port) != 3) return;
        if (id == id_ || set_ != set) return;               // ourselves, or another game
        if (paired_ && id != peerId_) return;               // a third cabinet: not ours
        const bool fresh = !paired_;
        peerId_ = id;
        peerAddr_ = fromAddr;
        peerPort_ = port;
        cabinet_ = id_ < id ? 0 : 1;
        paired_ = true;
        lastHeard_ = now();
        if (fresh) announce(true);                          // let it find us at once
    } else if (data[m] == 'P' && paired_ && size >= m + 1 + 8) {
        uint64_t id = 0;
        for (int i = 0; i < 8; i++) id = id << 8 | data[m + 1 + i];
        if (id != peerId_) return;
        const int words = (size - (m + 1 + 8)) / 2;
        if (words <= 0 || words > MAX_WORDS) return;
        std::vector<uint16_t> w((size_t)words);
        const uint8_t *p = data + m + 1 + 8;
        for (int i = 0; i < words; i++) w[(size_t)i] = (uint16_t)(p[2 * i] << 8 | p[2 * i + 1]);
        in_.push_back(std::move(w));
        lastHeard_ = now();
    }
}

void Link::send(const uint16_t *words, int count)
{
    if (!open_ || !paired_ || count <= 0 || count > MAX_WORDS) return;
    uint8_t buf[sizeof MAGIC + 8 + 2 * MAX_WORDS];
    const int m = (int)sizeof MAGIC - 1;
    memcpy(buf, MAGIC, (size_t)m);
    buf[m] = 'P';
    for (int i = 0; i < 8; i++) buf[m + 1 + i] = (uint8_t)(id_ >> (56 - 8 * i));
    uint8_t *p = buf + m + 1 + 8;
    for (int i = 0; i < count; i++) { p[2 * i] = (uint8_t)(words[i] >> 8); p[2 * i + 1] = (uint8_t)words[i]; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = peerAddr_;
    a.sin_port = htons((uint16_t)peerPort_);
    sendto(sock(data_), (const char *)buf, m + 1 + 8 + 2 * count, 0, (sockaddr *)&a, sizeof a);
}

bool Link::receive(std::vector<uint16_t> &words)
{
    if (in_.empty()) return false;
    words = std::move(in_.front());
    in_.pop_front();
    return true;
}
