/* bgapi_core.cpp -- see bgapi_core.h */
#include "bgapi_core.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace bgmon {

/* ------------------------------------------------------------------ tables */

namespace {

struct Index {
    std::unordered_map<uint64_t, const bgapi_msg_def_t *> byId;
    std::unordered_map<std::string, const bgapi_msg_def_t *> cmdByName;
    Index() {
        for (unsigned i = 0; i < bgapi_msg_def_count; ++i) {
            const bgapi_msg_def_t *d = &bgapi_msg_defs[i];
            byId[(uint64_t(d->id) << 2) | unsigned(d->kind)] = d;
            if (d->kind == BGAPI_KIND_CMD) {
                cmdByName[d->name] = d;
                cmdByName[d->name + 4] = d;    /* without "cmd_" */
            }
        }
    }
};

const Index &index() {
    static const Index idx;
    return idx;
}

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string msText(int64_t us) {
    return fmt("%.3f ms", double(us) / 1000.0);
}

} // namespace

const bgapi_msg_def_t *findDef(uint32_t msgId, bgapi_kind_t kind) {
    auto it = index().byId.find((uint64_t(msgId) << 2) | unsigned(kind));
    return it == index().byId.end() ? nullptr : it->second;
}

const bgapi_msg_def_t *findCommandByName(const std::string &name) {
    auto it = index().cmdByName.find(name);
    return it == index().cmdByName.end() ? nullptr : it->second;
}

const char *className(uint8_t classId) {
    for (unsigned i = 0; i < bgapi_class_def_count; ++i)
        if (bgapi_class_defs[i].class_id == classId) return bgapi_class_defs[i].name;
    return nullptr;
}

const char *statusName(uint16_t value) {
    for (unsigned i = 0; i < bgapi_status_def_count; ++i)
        if (bgapi_status_defs[i].value == value) return bgapi_status_defs[i].name;
    return nullptr;
}

/* ------------------------------------------------------------ StreamParser */

StreamParser::StreamParser(Dir dir, Emit emit, ExtraKnown extraKnown)
    : dir_(dir), emit_(std::move(emit)), extra_(std::move(extraKnown)) {}

bool StreamParser::firstByteOk(uint8_t b) const {
    if ((b & kDevTypeMask) != kDevTypeBt) return false;            /* Bluetooth, not encrypted */
    if (dir_ == Dir::HostToNcp && (b & kEventBit)) return false;    /* a host only sends commands */
    if ((b & 0x07u) > (kMaxPayload >> 8)) return false;             /* length > 256 */
    return true;
}

void StreamParser::feed(const uint8_t *data, size_t n, int64_t tUs) {
    if (n == 0) return;
    buf_.insert(buf_.end(), data, data + n);
    bufT_.insert(bufT_.end(), n, tUs);
    lastByteUs_ = tUs;
    process();
}

void StreamParser::process() {
    while (!buf_.empty()) {
        const uint8_t b0 = buf_[0];
        bool reject = !firstByteOk(b0);
        if (!reject) {
            if (buf_.size() < kHeaderLen) break;                    /* wait for the rest of the header */
            const uint32_t hdr = headerFromBytes(buf_.data());
            const uint16_t len = payloadLenOf(hdr);
            const uint32_t id = msgIdOf(hdr);
            const bgapi_kind_t kind = (dir_ == Dir::HostToNcp) ? BGAPI_KIND_CMD
                                      : ((b0 & kEventBit) ? BGAPI_KIND_EVT : BGAPI_KIND_RSP);
            const bool known = findDef(id, kind) != nullptr
                               || (kind == BGAPI_KIND_RSP && extra_ && extra_(id));
            if (!known || len > kMaxPayload) {
                reject = true;
            } else {
                const size_t total = kHeaderLen + len;
                if (buf_.size() < total) break;                     /* wait for the payload */
                emitJunk();
                Item it;
                it.type = ItemType::Frame;
                it.bytes.assign(buf_.begin(), buf_.begin() + long(total));
                it.tFirstUs = bufT_[0];
                it.tLastUs = bufT_[total - 1];
                buf_.erase(buf_.begin(), buf_.begin() + long(total));
                bufT_.erase(bufT_.begin(), bufT_.begin() + long(total));
                emit_(std::move(it));
                continue;
            }
        }
        if (reject) {
            /* Not a header start: the byte is unsynced. Same rule the host
             * library uses (sli_wait_for_bgapi_message), but the byte is kept
             * and shown instead of being discarded silently. */
            if (junk_.empty()) junkT0_ = bufT_[0];
            junkT1_ = bufT_[0];
            junk_.push_back(b0);
            buf_.erase(buf_.begin());
            bufT_.erase(bufT_.begin());
        }
    }
    emitJunk();
}

void StreamParser::emitJunk() {
    if (junk_.empty()) return;
    Item it;
    it.type = ItemType::Unsynced;
    it.bytes.swap(junk_);
    it.tFirstUs = junkT0_;
    it.tLastUs = junkT1_;
    emit_(std::move(it));
}

void StreamParser::poll(int64_t nowUs) {
    if (!buf_.empty() && nowUs - lastByteUs_ >= gapTimeoutUs) flush();
}

void StreamParser::flush() {
    emitJunk();
    if (buf_.empty()) return;
    Item it;
    it.type = ItemType::Truncated;
    it.bytes.swap(buf_);
    it.tFirstUs = bufT_.front();
    it.tLastUs = bufT_.back();
    bufT_.clear();
    emit_(std::move(it));
}

/* ------------------------------------------------------------------ Record */

void Record::addIssue(Severity s, const std::string &text) {
    issues.push_back(text);
    if (s > sev) sev = s;
}

void analyseFrame(Record &r) {
    if (r.type == ItemType::Unsynced) {
        r.name = fmt("unsynced bytes (%u)", unsigned(r.bytes.size()));
        r.addIssue(Severity::Warn, fmt("%u byte(s) that do not start a valid Bluetooth BGAPI header; "
                                       "a BGLib host discards these", unsigned(r.bytes.size())));
        return;
    }
    if (r.type == ItemType::Truncated) {
        if (r.bytes.size() >= kHeaderLen) {
            r.header = headerFromBytes(r.bytes.data());
            r.payloadLen = payloadLenOf(r.header);
            r.name = fmt("truncated frame (%u of %u bytes)", unsigned(r.bytes.size()),
                         unsigned(kHeaderLen + r.payloadLen));
        } else {
            r.name = fmt("truncated header (%u of 4 bytes)", unsigned(r.bytes.size()));
        }
        r.addIssue(Severity::Error, "frame stopped before its declared length (no byte arrived for the gap timeout)");
        return;
    }

    if (r.bytes.size() < kHeaderLen) {
        r.name = "short frame";
        r.addIssue(Severity::Error, fmt("only %u byte(s); a BGAPI header is 4 bytes", unsigned(r.bytes.size())));
        return;
    }
    const uint8_t b0 = r.bytes[0];
    r.header = headerFromBytes(r.bytes.data());
    r.payloadLen = payloadLenOf(r.header);
    const uint32_t id = msgIdOf(r.header);
    if (b0 & kEventBit) r.kind = BGAPI_KIND_EVT;
    else r.kind = (r.dir == Dir::HostToNcp) ? BGAPI_KIND_CMD : BGAPI_KIND_RSP;
    r.def = findDef(id, r.kind);

    const char *kp = r.kind == BGAPI_KIND_CMD ? "cmd" : r.kind == BGAPI_KIND_RSP ? "rsp" : "evt";
    if (r.def) {
        r.name = r.def->name;
    } else {
        const char *cn = className(classOf(r.header));
        r.name = fmt("%s_%s_unknown_0x%02x", kp, cn ? cn : "class?", messageOf(r.header));
    }

    if ((b0 & 0x38) != kDevTypeBt)
        r.addIssue(Severity::Error, fmt("device type bits = %u%u%u, Bluetooth is 100",
                                        (b0 >> 5) & 1, (b0 >> 4) & 1, (b0 >> 3) & 1));
    if (b0 & 0x40)
        r.addIssue(Severity::Error, "bit 6 (encrypted) is set; the NCP link does not use encrypted messages");
    if (r.dir == Dir::HostToNcp && r.kind == BGAPI_KIND_EVT)
        r.addIssue(Severity::Error, "event bit set on a host-to-NCP message");
    if (!r.def)
        r.addIssue(Severity::Warn, fmt("class 0x%02x / message 0x%02x is not a %s in this SDK's API",
                                       classOf(r.header), messageOf(r.header),
                                       r.kind == BGAPI_KIND_CMD ? "command" : r.kind == BGAPI_KIND_RSP ? "response" : "event"));

    const size_t present = r.bytes.size() - kHeaderLen;
    if (present != r.payloadLen)
        r.addIssue(Severity::Error, fmt("header length field says %u payload bytes, %u follow",
                                        unsigned(r.payloadLen), unsigned(present)));
    if (r.payloadLen > kMaxPayload)
        r.addIssue(Severity::Error, fmt("payload %u > %u bytes: BGLib rejects this header",
                                        unsigned(r.payloadLen), kMaxPayload));

    const uint8_t *pl = r.bytes.data() + kHeaderLen;
    const size_t avail = std::min<size_t>(present, r.payloadLen);

    if (r.kind == BGAPI_KIND_RSP && avail >= 2) {
        r.hasResult = true;
        r.result = uint16_t(pl[0] | (pl[1] << 8));
    }

    if (!r.def) return;
    const unsigned minLen = r.def->min_len;

    if (r.kind == BGAPI_KIND_RSP && r.payloadLen == 2 && minLen != 2) {
        /* sl_bgapi_set_error_response(): result only, for any command */
        r.genericError = true;
        if (r.result == 0)
            r.addIssue(Severity::Error, fmt("2-byte response with result 0; this response is %u bytes", minLen));
        else
            r.addIssue(Severity::Info, "generic error response (payload is the result code only)");
        return;
    }

    if (r.def->array_len_size == 0) {
        if (r.payloadLen != minLen)
            r.addIssue(Severity::Error, fmt("payload %u bytes, the API defines exactly %u",
                                            unsigned(r.payloadLen), minLen));
    } else if (r.payloadLen < minLen) {
        r.addIssue(Severity::Error, fmt("payload %u bytes, the API needs at least %u",
                                        unsigned(r.payloadLen), minLen));
    } else if (avail >= minLen) {
        const unsigned off = minLen - r.def->array_len_size;
        unsigned arr = pl[off];
        if (r.def->array_len_size == 2) arr |= unsigned(pl[off + 1]) << 8;
        if (r.payloadLen != minLen + arr)
            r.addIssue(Severity::Error, fmt("array length field = %u, so the payload should be %u bytes, not %u",
                                            arr, minLen + arr, unsigned(r.payloadLen)));
    }
}

std::string hexBytes(const std::vector<uint8_t> &b, size_t from, size_t count) {
    std::string s;
    const size_t end = (count == SIZE_MAX) ? b.size() : std::min(b.size(), from + count);
    static const char *hx = "0123456789ABCDEF";
    for (size_t i = from; i < end; ++i) {
        if (i != from) s += ' ';
        s += hx[b[i] >> 4];
        s += hx[b[i] & 15];
    }
    return s;
}

std::string asciiBytes(const std::vector<uint8_t> &b, size_t from, size_t count) {
    std::string s;
    const size_t end = (count == SIZE_MAX) ? b.size() : std::min(b.size(), from + count);
    for (size_t i = from; i < end; ++i) s += (b[i] >= 0x20 && b[i] < 0x7f) ? char(b[i]) : '.';
    return s;
}

std::string decodeText(const Record &r) {
    std::string s;
    const char *srcName = r.src == Source::Host ? "host application" : r.src == Source::Tool ? "this tool" : "NCP";
    s += fmt("#%llu  %s  from %s\n", (unsigned long long)r.seq,
             r.dir == Dir::HostToNcp ? "TX (to NCP)" : "RX (from NCP)", srcName);
    s += "Message : " + r.name + "\n";

    if (r.type == ItemType::Frame && r.bytes.size() >= kHeaderLen) {
        const uint8_t b0 = r.bytes[0];
        s += fmt("Header  : %s   (uint32 0x%08x)\n", hexBytes(r.bytes, 0, 4).c_str(), r.header);
        s += fmt("  byte0 : 0x%02x  type=%s  encrypted=%u  device=%u%u%u%s  len[10:8]=%u\n", b0,
                 (b0 & 0x80) ? "event" : "cmd/rsp", (b0 >> 6) & 1, (b0 >> 5) & 1, (b0 >> 4) & 1, (b0 >> 3) & 1,
                 ((b0 & 0x38) == 0x20) ? " (Bluetooth)" : "", b0 & 7);
        const char *cn = className(classOf(r.header));
        s += fmt("  length: %u   class: 0x%02x (%s)   message: 0x%02x\n", unsigned(r.payloadLen),
                 classOf(r.header), cn ? cn : "?", messageOf(r.header));
        if (r.def) {
            if (r.genericError)
                s += fmt("API size: %u bytes normally; this is the 2-byte error form\n", r.def->min_len);
            else if (r.def->array_len_size)
                s += fmt("API size: %u fixed + array (%s length field)\n", r.def->min_len,
                         r.def->array_len_size == 1 ? "uint8" : "uint16");
            else
                s += fmt("API size: %u bytes (fixed)\n", r.def->min_len);
        }
        if (r.hasResult) {
            const char *sn = statusName(r.result);
            s += fmt("Result  : 0x%04x %s\n", r.result, sn ? sn : "(unknown status)");
        }
    }
    if (r.matchSeq) {
        if (r.kind == BGAPI_KIND_RSP)
            s += fmt("Answers : #%llu  (%s after the command)\n", (unsigned long long)r.matchSeq, msText(r.rttUs).c_str());
        else
            s += fmt("Answered: by #%llu  (%s)\n", (unsigned long long)r.matchSeq, msText(r.rttUs).c_str());
    }
    if (!r.state.empty()) s += "State   : " + r.state + "\n";
    if (!r.route.empty()) s += "Route   : " + r.route + "\n";

    const size_t pfrom = (r.type == ItemType::Frame && r.bytes.size() >= kHeaderLen) ? kHeaderLen : 0;
    const size_t plen = r.bytes.size() - pfrom;
    s += fmt("%s (%u bytes):\n", pfrom ? "Payload" : "Bytes", unsigned(plen));
    for (size_t o = pfrom; o < r.bytes.size(); o += 16) {
        std::string h = hexBytes(r.bytes, o, 16);
        h.resize(16 * 3 - 1, ' ');
        s += fmt("  %04x  %s  %s\n", unsigned(o - pfrom), h.c_str(), asciiBytes(r.bytes, o, 16).c_str());
    }
    if (!r.issues.empty()) {
        s += "Checks  :\n";
        for (const auto &i : r.issues) s += "  ! " + i + "\n";
    } else if (r.type == ItemType::Frame) {
        s += "Checks  : header, device type and payload length match the API\n";
    }
    return s;
}

/* ----------------------------------------------------------------- Session */

Session::Session(Mode mode, Callbacks cb)
    : mode_(mode), cb_(std::move(cb)),
      ncpParser_(Dir::NcpToHost, [this](Item &&it) { onNcpItem(std::move(it)); },
                 [this](uint32_t id) { return isOutstandingId(id); }),
      hostParser_(Dir::HostToNcp, [this](Item &&it) { onHostItem(std::move(it)); }) {}

uint64_t Session::push(Record &&r, bool keepLive) {
    r.seq = nextSeq_++;
    if (r.type == ItemType::Frame) stats.frames++;
    else stats.unsyncedBytes += r.bytes.size();
    if (r.sev == Severity::Error) stats.errors++;
    else if (r.sev == Severity::Warn) stats.warnings++;
    const uint64_t seq = r.seq;
    if (cb_.added) cb_.added(r);
    if (keepLive) live_[seq] = std::move(r);
    return seq;
}

Record *Session::find(uint64_t seq) {
    auto it = live_.find(seq);
    return it == live_.end() ? nullptr : &it->second;
}

void Session::notifyUpdated(uint64_t seq) {
    Record *r = find(seq);
    if (r && cb_.updated) cb_.updated(*r);
}

bool Session::isOutstandingId(uint32_t id) const {
    for (const auto &p : pending_)
        if (p.msgId == id) return true;
    return false;
}

bool Session::toolBlocking() const {
    for (const auto &p : pending_)
        if (p.src == Source::Tool && !p.timedOut) return true;
    return false;
}

bool Session::anyLive() const {
    for (const auto &p : pending_)
        if (!p.timedOut) return true;
    return false;
}

void Session::fromNcp(const uint8_t *data, size_t n, int64_t tUs) {
    nowUs_ = tUs;
    ncpParser_.feed(data, n, tUs);
}

void Session::fromHost(const uint8_t *data, size_t n, int64_t tUs) {
    if (mode_ != Mode::Proxy) return;
    nowUs_ = tUs;
    hostParser_.feed(data, n, tUs);
}

void Session::onHostItem(Item &&it) {
    Record r;
    r.dir = Dir::HostToNcp;
    r.src = Source::Host;
    r.type = it.type;
    r.bytes = it.bytes;
    r.tUs = it.tFirstUs;
    analyseFrame(r);

    if (it.type == ItemType::Frame) {
        for (const auto &p : pending_)
            if (p.src == Source::Host && !p.timedOut) {
                r.addIssue(Severity::Warn, fmt("the host sent a new command while #%llu was still waiting for "
                                               "its response", (unsigned long long)p.seq));
                break;
            }
    }

    if (toolBlocking() || !held_.empty()) {
        r.route = "held: a command from this tool is waiting for its response";
        const uint64_t seq = push(std::move(r), true);
        held_.push_back(Held{std::move(it), seq});
        return;
    }
    const uint64_t seq = push(std::move(r), true);
    forwardHostItem(it, seq, -1);
}

void Session::forwardHostItem(const Item &it, uint64_t seq, int64_t heldSinceUs) {
    Record *r = find(seq);
    const bool ok = cb_.writeNcp && cb_.writeNcp(it.bytes.data(), it.bytes.size());
    if (r) {
        if (!ok) {
            r->addIssue(Severity::Error, "writing to the NCP port failed");
            r->route = "not delivered to the NCP";
        } else if (heldSinceUs >= 0) {
            r->route = "-> NCP (held " + msText(nowUs_ - heldSinceUs) + ")";
        } else {
            r->route = "-> NCP";
        }
        const bool isCmd = it.type == ItemType::Frame && ok;
        if (isCmd) {
            r->state = "awaiting response";
            pending_.push_back(Pending{seq, msgIdOf(r->header), Source::Host, nowUs_, false});
        }
        notifyUpdated(seq);
        if (!isCmd) live_.erase(seq);
    }
}

void Session::releaseHeld(int64_t nowUs) {
    (void)nowUs;
    if (toolBlocking()) return;
    while (!held_.empty()) {
        Held h = std::move(held_.front());
        held_.pop_front();
        forwardHostItem(h.item, h.seq, h.item.tFirstUs);
    }
}

uint64_t Session::sendFromTool(const std::vector<uint8_t> &frame, int64_t tUs) {
    nowUs_ = tUs;
    Record r;
    r.dir = Dir::HostToNcp;
    r.src = Source::Tool;
    r.type = ItemType::Frame;
    r.bytes = frame;
    r.tUs = tUs;
    analyseFrame(r);
    r.route = "queued";
    r.state = "waiting until no command is outstanding";
    const uint64_t seq = push(std::move(r), true);
    toolQueue_.push_back(seq);
    pumpToolQueue(tUs);
    return seq;
}

void Session::pumpToolQueue(int64_t nowUs) {
    while (!toolQueue_.empty() && !anyLive() && held_.empty()) {
        const uint64_t seq = toolQueue_.front();
        toolQueue_.pop_front();
        sendTool(seq, nowUs);
    }
}

void Session::sendTool(uint64_t seq, int64_t nowUs) {
    Record *r = find(seq);
    if (!r) return;
    const bool ok = cb_.writeNcp && cb_.writeNcp(r->bytes.data(), r->bytes.size());
    if (!ok) {
        r->addIssue(Severity::Error, "writing to the NCP port failed");
        r->route = "not sent";
        r->state.clear();
        notifyUpdated(seq);
        live_.erase(seq);
        return;
    }
    if (nowUs > r->tUs) r->route = "-> NCP (queued " + msText(nowUs - r->tUs) + ")";
    else r->route = "-> NCP";
    if (r->bytes.size() >= kHeaderLen && !(r->bytes[0] & kEventBit)) {
        r->state = "awaiting response";
        pending_.push_back(Pending{seq, msgIdOf(r->header), Source::Tool, nowUs, false});
        notifyUpdated(seq);
    } else {
        r->state = "no response expected (not a command header)";
        notifyUpdated(seq);
        live_.erase(seq);
    }
}

void Session::onNcpItem(Item &&it) {
    Record r;
    r.dir = Dir::NcpToHost;
    r.src = Source::Ncp;
    r.type = it.type;
    r.bytes = it.bytes;
    r.tUs = it.tFirstUs;
    analyseFrame(r);

    auto toHost = [&](Record &rec) {
        if (mode_ != Mode::Proxy) return;
        const bool ok = cb_.writeHost && cb_.writeHost(it.bytes.data(), it.bytes.size());
        rec.route = ok ? "-> host" : "not delivered to the host";
        if (!ok) rec.addIssue(Severity::Warn, "writing to the host (proxy) port failed");
    };

    if (it.type != ItemType::Frame || r.kind == BGAPI_KIND_EVT) {
        toHost(r);
        push(std::move(r), false);
        return;
    }

    /* response: the NCP answers commands strictly in order, so the oldest
     * outstanding command with the same ID is the one being answered. */
    const uint32_t id = msgIdOf(r.header);
    size_t i = 0;
    while (i < pending_.size() && pending_[i].msgId != id) ++i;

    if (i == pending_.size()) {
        std::string why = "unsolicited response: no command with this ID is waiting";
        for (const auto &p : pending_)
            if (p.src == Source::Host && mode_ == Mode::Proxy) {
                why += fmt("; the host's BGLib will take it as the answer to #%llu", (unsigned long long)p.seq);
                break;
            }
        r.addIssue(Severity::Error, why);
        toHost(r);
        push(std::move(r), false);
        return;
    }

    for (size_t j = 0; j < i; ++j) {
        if (Record *c = find(pending_[j].seq)) {
            c->state = "no response (a later command was answered first)";
            c->addIssue(Severity::Error, "the NCP never answered this command");
            stats.errors++;
            notifyUpdated(c->seq);
            live_.erase(c->seq);
        }
    }

    const Pending p = pending_[i];
    pending_.erase(pending_.begin(), pending_.begin() + long(i) + 1);

    r.matchSeq = p.seq;
    r.rttUs = it.tFirstUs - p.tSent;
    if (p.timedOut)
        r.addIssue(Severity::Warn, "late: arrived after the response timeout");
    if (p.src == Source::Tool) {
        if (mode_ == Mode::Proxy) r.route = "to this tool only (not forwarded to the host)";
    } else {
        toHost(r);
    }
    const bool isErr = r.hasResult && r.result != 0;
    const uint16_t result = r.result;
    const int64_t rtt = r.rttUs;
    const uint64_t rspSeq = push(std::move(r), false);

    if (Record *c = find(p.seq)) {
        c->matchSeq = rspSeq;
        c->rttUs = rtt;
        if (isErr) {
            const char *sn = statusName(result);
            c->state = fmt("answered: 0x%04x %s", result, sn ? sn : "");
        } else {
            c->state = "answered";
        }
        notifyUpdated(c->seq);
        live_.erase(c->seq);
    }
    releaseHeld(it.tFirstUs);
    pumpToolQueue(nowUs_);
}

void Session::tick(int64_t nowUs) {
    nowUs_ = nowUs;
    ncpParser_.poll(nowUs);
    hostParser_.poll(nowUs);

    for (size_t k = 0; k < pending_.size();) {
        Pending &p = pending_[k];
        if (!p.timedOut && nowUs - p.tSent >= responseTimeoutUs) {
            p.timedOut = true;
            if (Record *c = find(p.seq)) {
                c->state = "no response yet (" + msText(responseTimeoutUs) + "); a late one is still matched";
                c->addIssue(Severity::Warn, "no response within the response timeout");
                stats.warnings++;
                notifyUpdated(c->seq);
            }
        }
        if (p.timedOut && nowUs - p.tSent >= forgetAfterUs) {
            if (Record *c = find(p.seq)) {
                c->state = "no response";
                c->addIssue(Severity::Error, "the NCP never answered this command");
                stats.errors++;
                notifyUpdated(c->seq);
                live_.erase(c->seq);
            }
            pending_.erase(pending_.begin() + long(k));
            continue;
        }
        ++k;
    }
    releaseHeld(nowUs);
    pumpToolQueue(nowUs);
}

void Session::close(int64_t nowUs) {
    nowUs_ = nowUs;
    ncpParser_.flush();
    hostParser_.flush();
    for (const auto &p : pending_) {
        if (Record *c = find(p.seq)) {
            c->state = "no response (port closed)";
            notifyUpdated(c->seq);
        }
    }
    pending_.clear();
    for (auto seq : toolQueue_) {
        if (Record *c = find(seq)) {
            c->route = "not sent (port closed)";
            c->state.clear();
            notifyUpdated(seq);
        }
    }
    toolQueue_.clear();
    for (auto &h : held_) {
        if (Record *c = find(h.seq)) {
            c->route = "not delivered (port closed)";
            notifyUpdated(h.seq);
        }
    }
    held_.clear();
    live_.clear();
}

} // namespace bgmon
