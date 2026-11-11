/* test_proxy_bglib.cpp -- the proxy under the REAL Silicon Labs host library.
 *
 * Host app  : sl_bt_ncp_host.c + sl_bt_ncp_host_api.c + bt_transport_adapter.c
 *             (the TMS Configurator's own copies, unmodified), calling sl_bt_*
 * Proxy     : bgmon::Session in proxy mode
 * NCP       : a fake target that answers every command serially, at the exact
 *             API size, with result = CRC16 of the command bytes, and emits
 *             random events at random moments
 * Tool      : injects random commands (some answered only after the response
 *             timeout) while the host is busy
 *
 * Everything is single-threaded: when BGLib reads its UART, the world is
 * stepped until the bytes are there, with random chunking and random time.
 *
 * Pass criteria (any violation = a false reading somewhere):
 *  1. every sl_bt_* call returns the result the NCP computed for THAT call's
 *     bytes -> the host never received a response meant for the tool
 *  2. the host receives exactly the NCP's events, in order, and nothing else
 *  3. the NCP receives every host and tool frame intact, never interleaved
 *  4. the monitor's records: every response matched to the right command,
 *     no unsynced / truncated / error records, every tool command answered
 *  A sabotaged run (tool responses leaked to the host) must FAIL check 1/2,
 *  proving the harness can see the failure it is looking for.
 */
#include "bgapi_core.h"

extern "C" {
#include "sl_bt_ncp_host.h"
#include "bt_transport_adapter.h"
int32_t uartTx(uint32_t len, const uint8_t *data);
int32_t uartRx(uint32_t len, uint8_t *data);
int32_t uartRxPeek(void);
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <vector>

using namespace bgmon;
using Bytes = std::vector<uint8_t>;

static uint16_t crc16(const Bytes &b) {
    uint16_t c = 0xffff;
    for (uint8_t x : b) {
        c ^= uint16_t(x) << 8;
        for (int i = 0; i < 8; ++i) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}

static Bytes buildMsg(const bgapi_msg_def_t *d, std::mt19937 &rng, unsigned maxArr) {
    unsigned arr = d->array_len_size ? unsigned(rng() % (maxArr + 1)) : 0;
    unsigned len = d->min_len + arr;
    Bytes f(4 + len);
    uint32_t hdr = d->id | ((len & 0xffu) << 8) | ((len & 0x700u) >> 8);
    for (int i = 0; i < 4; ++i) f[i] = uint8_t(hdr >> (8 * i));
    for (unsigned i = 0; i < len; ++i) f[4 + i] = uint8_t(rng());
    if (d->array_len_size) {
        unsigned off = 4 + d->min_len - d->array_len_size;
        f[off] = uint8_t(arr);
        if (d->array_len_size == 2) f[off + 1] = 0;
    }
    return f;
}

static unsigned seedFromEnv() { const char *s = std::getenv("BGMON_SEED"); return s ? unsigned(std::strtoul(s, nullptr, 10)) : 12345u; }

struct World {
    std::mt19937 rng{seedFromEnv()};
    int64_t now = 0;
    Session *S = nullptr;
    bool sabotage = false;
    bool direct = false;

    std::deque<uint8_t> hostOut, hostIn, ncpIn, ncpOut;
    Bytes lastHostFrame;
    std::vector<Bytes> hostFrames, toolFrames, ncpEvents, hostEvents, ncpGot;
    Bytes ncpStreamForHost;   /* NCP output minus responses to tool commands */
    Bytes proxyToHost;        /* what the proxy actually wrote to the host */
    std::deque<bool> cmdIsTool; /* per command received by the NCP */
    size_t hiN = 0, tiN = 0; unsigned ambiguous = 0;
    std::map<uint64_t, Record> recs;

    /* fake NCP */
    Bytes ncpRx;
    struct Job { Bytes rsp; int64_t readyAt; bool tool; };
    std::deque<Job> jobs;
    int64_t lastReady = 0;
    unsigned lateToolCmds = 0, toolSent = 0, toolBudget = 0;

    std::vector<const bgapi_msg_def_t *> evtDefs, toolCmdDefs;

    void init() {
        for (unsigned i = 0; i < bgapi_msg_def_count; ++i) {
            const bgapi_msg_def_t *d = &bgapi_msg_defs[i];
            if (d->kind == BGAPI_KIND_EVT && d->min_len + 16u <= kMaxPayload) evtDefs.push_back(d);
            if (d->kind == BGAPI_KIND_CMD && d->min_len >= 3 && d->min_len + 16u <= kMaxPayload) toolCmdDefs.push_back(d);
        }
    }

    void ncpConsume() {
        while (!ncpIn.empty()) { ncpRx.push_back(ncpIn.front()); ncpIn.pop_front(); }
        for (;;) {
            if (ncpRx.size() < 4) return;
            uint32_t hdr = headerFromBytes(ncpRx.data());
            size_t total = 4 + payloadLenOf(hdr);
            if (ncpRx.size() < total) return;
            Bytes cmd(ncpRx.begin(), ncpRx.begin() + long(total));
            ncpRx.erase(ncpRx.begin(), ncpRx.begin() + long(total));
            ncpGot.push_back(cmd);
            {
                /* classify by send order (pointers into each sender's log) */
                bool h = hiN < hostFrames.size() && hostFrames[hiN] == cmd;
                bool t = tiN < toolFrames.size() && toolFrames[tiN] == cmd;
                if (h && t) ambiguous++;
                if (t && !h) { tiN++; cmdIsTool.push_back(true); }
                else { if (h) hiN++; cmdIsTool.push_back(false); }
            }
            const bgapi_msg_def_t *rd = findDef(msgIdOf(hdr), BGAPI_KIND_RSP);
            Bytes rsp;
            if (!rd) {   /* unknown command: generic error response */
                rsp = {uint8_t(hdr & 0xff & ~0x07), 0x02, uint8_t(hdr >> 16), uint8_t(hdr >> 24), 0x0f, 0x00};
            } else {
                rsp = buildMsg(rd, rng, 8);
                uint16_t c = crc16(cmd);
                rsp[4] = uint8_t(c); rsp[5] = uint8_t(c >> 8);
            }
            /* marker: a tool user_message starting 0xEE is answered after the response timeout */
            bool slow = msgIdOf(hdr) == 0x00ff0020u && cmd.size() > 5 && cmd[5] == 0xEE;
            int64_t delay = slow ? 2600000 : int64_t(100 + rng() % 4000);
            if (slow) lateToolCmds++;
            int64_t start = std::max(now, lastReady);
            lastReady = start + delay;
            jobs.push_back(Job{rsp, lastReady, cmdIsTool.back()});
        }
    }

    void step() {
        now += int64_t(rng() % 3000);
        if (!hostOut.empty()) {
            size_t n = std::min<size_t>(hostOut.size(), 1 + rng() % 32);
            Bytes c(hostOut.begin(), hostOut.begin() + long(n));
            hostOut.erase(hostOut.begin(), hostOut.begin() + long(n));
            S->fromHost(c.data(), c.size(), now);
        }
        ncpConsume();
        while (!jobs.empty() && jobs.front().readyAt <= now) {
            ncpOut.insert(ncpOut.end(), jobs.front().rsp.begin(), jobs.front().rsp.end());
            if (!jobs.front().tool) ncpStreamForHost.insert(ncpStreamForHost.end(), jobs.front().rsp.begin(), jobs.front().rsp.end());
            jobs.pop_front();
        }
        if (rng() % 40 == 0) {
            Bytes e = buildMsg(evtDefs[rng() % evtDefs.size()], rng, 12);
            ncpEvents.push_back(e);
            ncpStreamForHost.insert(ncpStreamForHost.end(), e.begin(), e.end());
            ncpOut.insert(ncpOut.end(), e.begin(), e.end());
        }
        if (!ncpOut.empty()) {
            size_t n = std::min<size_t>(ncpOut.size(), 1 + rng() % 64);
            Bytes c(ncpOut.begin(), ncpOut.begin() + long(n));
            ncpOut.erase(ncpOut.begin(), ncpOut.begin() + long(n));
            S->fromNcp(c.data(), c.size(), now);
        }
        if (toolSent < toolBudget && rng() % 25 == 0) {
            Bytes f;
            if (rng() % 15 == 0) {   /* slow user message */
                f = {0x20, 0x03, 0xff, 0x00, 0x02, 0xEE, uint8_t(rng())};
            } else {
                f = buildMsg(toolCmdDefs[rng() % toolCmdDefs.size()], rng, 8);
            }
            toolFrames.push_back(f);
            toolSent++;
            S->sendFromTool(f, now);
        }
        S->tick(now);
    }
};

static World *W = nullptr;

extern "C" int32_t uartTx(uint32_t len, const uint8_t *data) {
    W->lastHostFrame.assign(data, data + len);
    W->hostFrames.push_back(W->lastHostFrame);
    W->hostOut.insert(W->hostOut.end(), data, data + len);
    return 0;
}
extern "C" int32_t uartRx(uint32_t len, uint8_t *data) {
    long guard = 0;
    while (W->hostIn.size() < len) {
        W->step();
        if (++guard > 5000000) { std::printf("FATAL: host starved\n"); std::exit(2); }
    }
    for (uint32_t i = 0; i < len; ++i) { data[i] = W->hostIn.front(); W->hostIn.pop_front(); }
    return int32_t(len);
}
extern "C" int32_t uartRxPeek(void) {
    W->step();
    return int32_t(W->hostIn.size());
}

static void popEvents() {
    sl_bt_msg_t e;
    while (sl_bt_pop_event(&e) == SL_STATUS_OK) {
        uint32_t len = SL_BT_MSG_LEN(e.header);
        Bytes b(4 + len);
        std::memcpy(b.data(), &e.header, 4);
        std::memcpy(b.data() + 4, &e.data, len);
        W->hostEvents.push_back(b);
    }
}

struct Result { int fails = 0; unsigned calls = 0, mismatches = 0; };

static Result runProxy(bool sabotage, unsigned calls, unsigned toolBudget) {
    World w;
    W = &w;
    w.init();
    w.sabotage = sabotage;
    w.toolBudget = toolBudget;
    Session::Callbacks cb;
    cb.writeNcp = [&](const uint8_t *d, size_t n) { w.ncpIn.insert(w.ncpIn.end(), d, d + n); return true; };
    cb.writeHost = [&](const uint8_t *d, size_t n) { w.hostIn.insert(w.hostIn.end(), d, d + n); w.proxyToHost.insert(w.proxyToHost.end(), d, d + n); return true; };
    cb.added = [&](const Record &r) {
        w.recs[r.seq] = r;
        if (w.sabotage && r.src == Source::Ncp && r.matchSeq && w.recs[r.matchSeq].src == Source::Tool)
            w.hostIn.insert(w.hostIn.end(), r.bytes.begin(), r.bytes.end());   /* leak tool responses */
    };
    cb.updated = [&](const Record &r) { w.recs[r.seq] = r; };
    Session S(Session::Mode::Proxy, cb);
    w.S = &S;

    static bool inited = false;
    if (!inited) { sl_bt_api_initialize_nonblock(bt_transport_tx, uartRx, uartRxPeek); inited = true; }
    { sl_bt_msg_t e; while (sl_bt_pop_event(&e) == SL_STATUS_OK) {} }

    Result res;
    std::mt19937 hr(99);
    for (unsigned i = 0; i < calls; ++i) {
        sl_status_t st = 0;
        uint8_t buf[300]; size_t outLen = 0; uint16_t u16 = 0; uint8_t u8 = 0; bd_addr a{};
        uint8_t data[64]; for (auto &x : data) x = uint8_t(hr());
        switch (hr() % 10) {
        case 0: st = sl_bt_system_hello(); break;
        case 1: st = sl_bt_system_get_identity_address(&a, &u8); break;
        case 2: st = sl_bt_gatt_set_max_mtu(uint16_t(23 + hr() % 225), &u16); break;
        case 3: st = sl_bt_connection_close(uint8_t(hr())); break;
        case 4: st = sl_bt_scanner_stop(); break;
        case 5: st = sl_bt_gatt_write_characteristic_value_without_response(uint8_t(hr()), uint16_t(hr()), 1 + hr() % 60, data, &u16); break;
        case 6: data[0] &= 0x7f; st = sl_bt_user_message_to_target(1 + hr() % 8, data, sizeof(buf), &outLen, buf); break;
        case 7: st = sl_bt_system_get_random_data(uint8_t(1 + hr() % 16), sizeof(buf), &outLen, buf); break;
        case 8: for (auto &x : a.addr) x = uint8_t(hr()); st = sl_bt_connection_open(a, 0, 1, &u8); break;
        case 9: st = sl_bt_gatt_server_write_attribute_value(uint16_t(hr()), 0, 1 + hr() % 40, data); break;
        }
        res.calls++;
        if (st != crc16(w.lastHostFrame)) res.mismatches++;
        popEvents();
    }
    /* drain: let the tool queue, late answers and events finish */
    for (int k = 0; k < 4000000 && (S.toolQueued() || S.outstanding() || !w.jobs.empty() || !w.ncpOut.empty() || !w.hostIn.empty()); ++k) {
        w.step();
        popEvents();
    }
    for (int k = 0; k < 2000; ++k) { w.step(); popEvents(); }
    S.close(w.now);

    /* ---- checks ---- */
    auto fail = [&](const char *what) { res.fails++; std::printf("  FAIL: %s\n", what); };
    if (res.mismatches) fail("host received a response that does not belong to its command");
    if (w.proxyToHost != w.ncpStreamForHost)
    {
        fail("bytes the proxy wrote to the host != NCP output minus the tool's responses");
        size_t i = 0; while (i < w.proxyToHost.size() && i < w.ncpStreamForHost.size() && w.proxyToHost[i] == w.ncpStreamForHost[i]) i++;
        std::printf("    sizes %zu vs %zu, first difference at byte %zu:", w.proxyToHost.size(), w.ncpStreamForHost.size(), i);
        for (size_t k = i; k < i + 12 && k < w.proxyToHost.size(); ++k) std::printf(" %02X", w.proxyToHost[k]);
        std::printf("  |");
        for (size_t k = i; k < i + 12 && k < w.ncpStreamForHost.size(); ++k) std::printf(" %02X", w.ncpStreamForHost[k]);
        std::printf("\n");
        unsigned amb = 0; for (auto &t : w.toolFrames) for (auto &h : w.hostFrames) if (t == h) { amb++; break; }
        std::printf("    tool frames byte-identical to some host frame: %u, ambiguous at NCP: %u\n", amb, w.ambiguous);
    }
    /* BGLib side: every event it delivered is an NCP event, in order. BGLib's own
     * 30-entry queue drops events while the host is blocked; count those. */
    {
        size_t k = 0; bool ok = true;
        for (auto &e : w.hostEvents) {
            if (!(e[0] & 0x80)) { ok = false; break; }
            while (k < w.ncpEvents.size() && w.ncpEvents[k] != e) k++;
            if (k == w.ncpEvents.size()) { ok = false; break; }
            k++;
        }
        if (!ok) fail("BGLib delivered an event that is not the NCP's next event (or a response as an event)");
        std::printf("  BGLib received %zu of %zu events (%zu dropped by BGLib's own %d-entry queue while blocked)\n",
                    w.hostEvents.size(), w.ncpEvents.size(), w.ncpEvents.size() - w.hostEvents.size(), SL_BT_API_QUEUE_LEN);
    }
    std::vector<Bytes> gotHost, gotTool;
    {
        size_t hi = 0, ti = 0; bool ok = true;
        for (auto &f : w.ncpGot) {
            if (hi < w.hostFrames.size() && f == w.hostFrames[hi]) hi++;
            else if (ti < w.toolFrames.size() && f == w.toolFrames[ti]) ti++;
            else ok = false;
        }
        if (!ok || hi != w.hostFrames.size() || ti != w.toolFrames.size() || !w.ncpRx.empty())
            fail("NCP did not receive exactly the host and tool frames, intact and in order");
    }
    unsigned rsp = 0, rspMatched = 0, bad = 0, toolAnswered = 0, toolCmds = 0, late = 0, hostCmds = 0, hostAnswered = 0;
    for (auto &kv : w.recs) {
        const Record &r = kv.second;
        if (r.type != ItemType::Frame) bad++;
        if (r.sev == Severity::Error) bad++;
        if (r.src == Source::Ncp && r.kind == BGAPI_KIND_RSP) {
            rsp++;
            if (r.matchSeq && w.recs[r.matchSeq].matchSeq == r.seq) rspMatched++;
            for (auto &i : r.issues) if (i.find("late") == 0) late++;
        }
        if (r.src == Source::Tool) { toolCmds++; if (r.state.rfind("answered", 0) == 0) toolAnswered++; }
        if (r.src == Source::Host) { hostCmds++; if (r.state.rfind("answered", 0) == 0) hostAnswered++; }
    }
    if (bad) fail("monitor produced unsynced/truncated/error records");
    if (rsp != rspMatched) fail("a response record is not linked to its command");
    if (toolAnswered != toolCmds) fail("a tool command did not end up answered");
    if (hostAnswered != hostCmds) fail("a host command did not end up answered");
    if (late != w.lateToolCmds) fail("late responses not reported as late");

    std::printf("  host calls %u, host result mismatches %u | tool commands %u (answered %u, late %u of %u slow) | "
                "NCP events %zu, host got %zu | responses %u matched %u | bad records %u\n",
                res.calls, res.mismatches, toolCmds, toolAnswered, late, w.lateToolCmds,
                w.ncpEvents.size(), w.hostEvents.size(), rsp, rspMatched, bad);
    W = nullptr;
    return res;
}

static int runDirect() {
    /* Direct mode: the tool is the only sender. Fire 400 commands at once:
     * they must go out one at a time and each response must match. */
    World w; W = &w; w.init(); w.direct = true;
    std::map<uint64_t, Record> recs;
    Session::Callbacks cb;
    cb.writeNcp = [&](const uint8_t *d, size_t n) { w.ncpIn.insert(w.ncpIn.end(), d, d + n); return true; };
    cb.added = [&](const Record &r) { recs[r.seq] = r; };
    cb.updated = [&](const Record &r) { recs[r.seq] = r; };
    Session S(Session::Mode::Direct, cb);
    w.S = &S;
    std::vector<Bytes> sent;
    for (int i = 0; i < 400; ++i) {
        Bytes f = buildMsg(w.toolCmdDefs[w.rng() % w.toolCmdDefs.size()], w.rng, 8);
        sent.push_back(f);
        S.sendFromTool(f, 0);
    }
    for (int k = 0; k < 2000000 && (S.toolQueued() || S.outstanding() || !w.jobs.empty() || !w.ncpOut.empty()); ++k) w.step();
    int fails = 0;
    unsigned answered = 0, wrong = 0;
    for (auto &kv : recs) {
        const Record &r = kv.second;
        if (r.src == Source::Tool && r.state.rfind("answered", 0) == 0) {
            const Record &rr = recs[r.matchSeq];
            if (rr.result != crc16(r.bytes)) wrong++;   /* the NCP's result encodes the command it answered */
            answered++;
        }
    }
    if (answered != 400 || wrong) { fails++; std::printf("  FAIL direct: answered %u/400, wrong match %u\n", answered, wrong); }
    if (w.ncpGot != sent) { fails++; std::printf("  FAIL direct: NCP did not receive the 400 frames in order\n"); }
    std::printf("  direct mode: 400 queued commands, %u answered, %u matched to the wrong command\n", answered, wrong);
    W = nullptr;
    return fails;
}

int main() {
    int fails = 0;
    std::printf("proxy, 3000 host calls, 300 tool injections:\n");
    Result r = runProxy(false, 3000, 300);
    fails += r.fails;
    std::printf("direct mode:\n");
    fails += runDirect();
    std::printf("sabotaged proxy (tool responses leaked to host) -- must be detected:\n");
    Result s = runProxy(true, 600, 120);
    bool detected = s.fails > 0;
    std::printf("  %s\n", detected ? "detected (good): the harness sees leaked responses" : "NOT DETECTED -- harness is blind");
    if (!detected) fails++;
    std::printf("%s\n", fails ? "PROXY TEST FAILED" : "PROXY TEST PASSED");
    return fails ? 1 : 0;
}
