/* bgapi_core.h -- Qt-free BGAPI monitor core.
 *
 *  StreamParser  byte-exact framing of one direction of the UART stream.
 *                Never drops a byte silently: every byte ends up either in a
 *                frame or in an "unsynced" / "truncated" item.
 *  Session       command/response bookkeeping (FIFO, like the NCP target
 *                processes them) and, in proxy mode, transparent forwarding
 *                between the host application and the NCP with safe
 *                injection of the tool's own commands.
 */
#ifndef BGAPI_CORE_H
#define BGAPI_CORE_H

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "bgapi_tables.h"

namespace bgmon {

/* Values from the SDK (sl_bgapi.h); repeated here so the core does not depend
 * on the SDK's C headers at compile time. Checked against them in tests. */
constexpr uint8_t  kDevTypeMask   = 0x78;   /* bits 6..3: encrypted + device type */
constexpr uint8_t  kDevTypeBt     = 0x20;   /* sl_bgapi_dev_type_bt */
constexpr uint8_t  kEventBit      = 0x80;   /* sl_bgapi_msg_type_evt */
constexpr uint32_t kMsgIdMask     = 0xffff00f8u; /* SL_BGAPI_MSG_ID */
constexpr unsigned kHeaderLen     = 4;      /* SL_BGAPI_MSG_HEADER_LEN */
constexpr unsigned kMaxPayload    = 256;    /* SL_BGAPI_MAX_PAYLOAD_SIZE */

inline uint32_t headerFromBytes(const uint8_t *b) {
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
inline uint16_t payloadLenOf(uint32_t hdr) { return uint16_t(((hdr & 0x7u) << 8) | ((hdr & 0xff00u) >> 8)); }
inline uint32_t msgIdOf(uint32_t hdr) { return hdr & kMsgIdMask; }
inline uint8_t classOf(uint32_t hdr) { return uint8_t(hdr >> 16); }
inline uint8_t messageOf(uint32_t hdr) { return uint8_t(hdr >> 24); }

const bgapi_msg_def_t *findDef(uint32_t msgId, bgapi_kind_t kind);
const bgapi_msg_def_t *findCommandByName(const std::string &name); /* "system_hello" or "cmd_system_hello" */
const char *className(uint8_t classId);       /* nullptr if unknown */
const char *statusName(uint16_t value);       /* nullptr if unknown */

enum class Dir { HostToNcp, NcpToHost };
enum class Source { Host, Tool, Ncp };
enum class ItemType { Frame, Unsynced, Truncated };
enum class Severity { Ok = 0, Info = 1, Warn = 2, Error = 3 };

struct Item {
    ItemType type = ItemType::Frame;
    std::vector<uint8_t> bytes;
    int64_t tFirstUs = 0;
    int64_t tLastUs = 0;
};

class StreamParser {
public:
    using Emit = std::function<void(Item &&)>;
    using ExtraKnown = std::function<bool(uint32_t msgId)>;

    StreamParser(Dir dir, Emit emit, ExtraKnown extraKnown = ExtraKnown());

    void feed(const uint8_t *data, size_t n, int64_t tUs);
    /* Emit a held partial frame as Truncated once no byte arrived for gapTimeoutUs. */
    void poll(int64_t nowUs);
    /* Emit whatever is held (port closing). */
    void flush();
    bool holding() const { return !buf_.empty(); }

    int64_t gapTimeoutUs = 200000;

private:
    void process();
    void emitJunk();
    bool firstByteOk(uint8_t b) const;

    Dir dir_;
    Emit emit_;
    ExtraKnown extra_;
    std::vector<uint8_t> buf_;      /* candidate frame bytes, buf_[0] is a header start */
    std::vector<int64_t> bufT_;
    std::vector<uint8_t> junk_;
    int64_t junkT0_ = 0, junkT1_ = 0;
    int64_t lastByteUs_ = 0;
};

struct Record {
    uint64_t seq = 0;
    Dir dir = Dir::HostToNcp;
    Source src = Source::Ncp;
    ItemType type = ItemType::Frame;
    std::vector<uint8_t> bytes;
    int64_t tUs = 0;

    /* frames only */
    uint32_t header = 0;
    uint16_t payloadLen = 0;
    bgapi_kind_t kind = BGAPI_KIND_CMD;
    const bgapi_msg_def_t *def = nullptr;
    bool hasResult = false;
    uint16_t result = 0;
    bool genericError = false;

    std::string name;                 /* "cmd_system_hello", "unknown 0x..", "unsynced bytes" */
    std::vector<std::string> issues;  /* protocol problems found in this record */
    Severity sev = Severity::Ok;
    std::string route;                /* where the bytes went */
    std::string state;                /* commands: awaiting / answered / no response */
    uint64_t matchSeq = 0;            /* cmd -> its response, rsp -> its command */
    int64_t rttUs = -1;

    void addIssue(Severity s, const std::string &text);
};

/* Header/payload checks against the SDK's struct sizes. Fills def, kind,
 * payloadLen, result, name, issues. Used for every frame, including the ones
 * typed into the Send panel. */
void analyseFrame(Record &r);

std::string hexBytes(const std::vector<uint8_t> &b, size_t from = 0, size_t count = SIZE_MAX);
std::string asciiBytes(const std::vector<uint8_t> &b, size_t from = 0, size_t count = SIZE_MAX);
std::string decodeText(const Record &r);   /* multi-line breakdown for the decoding pane */

class Session {
public:
    enum class Mode { Direct, Proxy };
    struct Callbacks {
        std::function<bool(const uint8_t *, size_t)> writeNcp;
        std::function<bool(const uint8_t *, size_t)> writeHost;   /* proxy mode only */
        std::function<void(const Record &)> added;
        std::function<void(const Record &)> updated;
    };

    Session(Mode mode, Callbacks cb);

    void fromNcp(const uint8_t *data, size_t n, int64_t tUs);
    void fromHost(const uint8_t *data, size_t n, int64_t tUs);
    /* Queue a frame from the Send panel. Sent as soon as no command is waiting
     * for its response. Returns the record's sequence number. */
    uint64_t sendFromTool(const std::vector<uint8_t> &frame, int64_t tUs);
    void tick(int64_t nowUs);
    void close(int64_t nowUs);

    Mode mode() const { return mode_; }
    size_t toolQueued() const { return toolQueue_.size(); }
    size_t outstanding() const { return pending_.size(); }

    int64_t responseTimeoutUs = 2000000;   /* flag + stop blocking the host */
    int64_t forgetAfterUs = 30000000;      /* give up matching a late response */

    struct Stats { uint64_t frames = 0, unsyncedBytes = 0, errors = 0, warnings = 0; } stats;

private:
    struct Pending { uint64_t seq; uint32_t msgId; Source src; int64_t tSent; bool timedOut; };
    struct Held { Item item; uint64_t seq; };

    uint64_t push(Record &&r, bool keepLive);
    Record *find(uint64_t seq);
    void notifyUpdated(uint64_t seq);
    void onNcpItem(Item &&it);
    void onHostItem(Item &&it);
    bool toolBlocking() const;
    bool anyLive() const;
    void forwardHostItem(const Item &it, uint64_t seq, int64_t heldSinceUs);
    void releaseHeld(int64_t nowUs);
    void pumpToolQueue(int64_t nowUs);
    void sendTool(uint64_t seq, int64_t nowUs);
    bool isOutstandingId(uint32_t id) const;

    Mode mode_;
    Callbacks cb_;
    StreamParser ncpParser_;
    StreamParser hostParser_;
    std::map<uint64_t, Record> live_;    /* records whose state can still change */
    std::deque<Pending> pending_;
    std::deque<Held> held_;
    std::deque<uint64_t> toolQueue_;
    uint64_t nextSeq_ = 1;
    int64_t nowUs_ = 0;
};

} // namespace bgmon

#endif
