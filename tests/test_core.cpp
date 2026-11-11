/* test_core.cpp -- parser / analyser tests (no hardware, no Qt).
 * Every property that matters for "no false readings" is asserted:
 *   - every SDK message, built at its exact API size, parses as one clean frame
 *   - byte conservation: the emitted items concatenate back to the input stream,
 *     for any chunking and any amount of noise
 *   - wrong lengths, generic error responses, truncation are reported
 */
#include "bgapi_core.h"

extern "C" {
#include "sl_bgapi.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace bgmon;

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static_assert(kMaxPayload == SL_BGAPI_MAX_PAYLOAD_SIZE, "max payload differs from SDK");
static_assert(kHeaderLen == SL_BGAPI_MSG_HEADER_LEN, "header length differs from SDK");
static_assert(kDevTypeBt == sl_bgapi_dev_type_bt, "device type differs from SDK");
static_assert(kEventBit == sl_bgapi_msg_type_evt, "event bit differs from SDK");

/* Build a frame for a definition: exact API size, optional array of arrLen bytes. */
static std::vector<uint8_t> build(const bgapi_msg_def_t *d, unsigned arrLen, std::mt19937 &rng) {
    unsigned len = d->min_len + (d->array_len_size ? arrLen : 0);
    std::vector<uint8_t> f(4 + len);
    uint32_t hdr = d->id | ((len & 0xffu) << 8) | ((len & 0x700u) >> 8);
    for (int i = 0; i < 4; ++i) f[i] = uint8_t(hdr >> (8 * i));
    for (unsigned i = 0; i < len; ++i) f[4 + i] = uint8_t(rng());
    if (d->array_len_size) {
        unsigned off = 4 + d->min_len - d->array_len_size;
        f[off] = uint8_t(arrLen);
        if (d->array_len_size == 2) f[off + 1] = uint8_t(arrLen >> 8);
    }
    if (d->kind == BGAPI_KIND_RSP) { f[4] = 0; f[5] = 0; }   /* result OK keeps the length rules strict */
    // SL_BGAPI macros agree with ours
    if (SL_BGAPI_MSG_LEN(hdr) != payloadLenOf(hdr) || SL_BGAPI_MSG_ID(hdr) != msgIdOf(hdr)) {
        ++g_fail; std::printf("FAIL macro mismatch %s\n", d->name);
    }
    return f;
}

struct Collector {
    std::vector<Item> items;
    StreamParser::Emit emit() { return [this](Item &&i) { items.push_back(std::move(i)); }; }
};

static Record recordFor(Dir dir, const Item &it) {
    Record r; r.dir = dir; r.type = it.type; r.bytes = it.bytes; analyseFrame(r); return r;
}

static void testEveryMessage() {
    std::mt19937 rng(1);
    unsigned n = 0;
    for (unsigned i = 0; i < bgapi_msg_def_count; ++i) {
        const bgapi_msg_def_t *d = &bgapi_msg_defs[i];
        const Dir dir = d->kind == BGAPI_KIND_CMD ? Dir::HostToNcp : Dir::NcpToHost;
        for (unsigned arr : {0u, 1u, 7u}) {
            if (!d->array_len_size && arr) continue;
            if (d->min_len + arr > kMaxPayload) continue;
            if (d->kind == BGAPI_KIND_RSP && d->min_len + arr == 2 && d->min_len != 2) continue;
            auto f = build(d, arr, rng);
            Collector c; StreamParser p(dir, c.emit());
            p.feed(f.data(), f.size(), 10);
            CHECK(c.items.size() == 1 && c.items[0].type == ItemType::Frame && c.items[0].bytes == f,
                  "%s arr=%u not parsed as one frame", d->name, arr);
            if (c.items.size() == 1) {
                Record r = recordFor(dir, c.items[0]);
                CHECK(r.def == d, "%s resolved to %s", d->name, r.def ? r.def->name : "null");
                CHECK(r.issues.empty(), "%s arr=%u issues: %s", d->name, arr, r.issues.empty() ? "" : r.issues[0].c_str());
            }
            ++n;
        }
    }
    std::printf("every-message: %u frames checked over %u definitions\n", n, bgapi_msg_def_count);
}

static void testKnownSizes() {
    auto def = [](const char *name) {
        for (unsigned i = 0; i < bgapi_msg_def_count; ++i)
            if (!std::strcmp(bgapi_msg_defs[i].name, name)) return &bgapi_msg_defs[i];
        return (const bgapi_msg_def_t *)nullptr;
    };
    /* sizes from the Silicon Labs API reference */
    struct { const char *n; unsigned len; unsigned arr; } k[] = {
        {"cmd_system_hello", 0, 0}, {"rsp_system_hello", 2, 0}, {"evt_system_boot", 18, 0},
        {"cmd_connection_open", 8, 0}, {"rsp_connection_open", 3, 0},
        {"cmd_gatt_write_characteristic_value", 4, 1}, {"rsp_user_message_to_target", 3, 1},
        {"evt_scanner_legacy_advertisement_report", 0, 0} /* checked below for >0 only */,
    };
    for (auto &e : k) {
        const bgapi_msg_def_t *d = def(e.n);
        CHECK(d != nullptr, "%s missing", e.n);
        if (d && e.len) CHECK(d->min_len == e.len && d->array_len_size == e.arr, "%s size %u/%u", e.n, d->min_len, d->array_len_size);
    }
    CHECK(def("evt_scanner_legacy_advertisement_report") && def("evt_scanner_legacy_advertisement_report")->array_len_size == 1,
          "legacy adv report should end in uint8array");
    CHECK(std::string(className(0x01)) == "system" && std::string(className(0xff)) == "user", "class names");
    CHECK(std::string(statusName(0)) == "SL_STATUS_OK", "status 0");
    CHECK(findCommandByName("system_hello") == def("cmd_system_hello"), "find by name");
}

static void testConservationFuzz() {
    std::mt19937 rng(7);
    std::vector<const bgapi_msg_def_t *> n2h;
    for (unsigned i = 0; i < bgapi_msg_def_count; ++i)
        if (bgapi_msg_defs[i].kind != BGAPI_KIND_CMD && bgapi_msg_defs[i].min_len + 8u <= kMaxPayload) n2h.push_back(&bgapi_msg_defs[i]);
    unsigned framesIn = 0, framesOut = 0, iters = 2000;
    for (unsigned it = 0; it < iters; ++it) {
        std::vector<uint8_t> stream;
        std::vector<std::vector<uint8_t>> frames;
        bool noisy = (it % 3) == 0;
        for (int k = 0; k < 20; ++k) {
            if (noisy && rng() % 4 == 0) {           /* noise that cannot start a header */
                int nn = 1 + rng() % 5;
                for (int q = 0; q < nn; ++q) { uint8_t b; do b = uint8_t(rng()); while ((b & 0x78) == 0x20); stream.push_back(b); }
            }
            const bgapi_msg_def_t *d = n2h[rng() % n2h.size()];
            auto f = build(d, d->array_len_size ? rng() % 8 : 0, rng);
            if (d->kind == BGAPI_KIND_RSP && f.size() == 6 && d->min_len != 2) continue;
            frames.push_back(f);
            stream.insert(stream.end(), f.begin(), f.end());
        }
        Collector c; StreamParser p(Dir::NcpToHost, c.emit());
        size_t pos = 0;
        while (pos < stream.size()) {
            size_t chunk = 1 + rng() % 40;
            if (pos + chunk > stream.size()) chunk = stream.size() - pos;
            p.feed(stream.data() + pos, chunk, int64_t(pos));
            pos += chunk;
        }
        p.flush();
        std::vector<uint8_t> back;
        std::vector<std::vector<uint8_t>> got;
        for (auto &i : c.items) {
            back.insert(back.end(), i.bytes.begin(), i.bytes.end());
            if (i.type == ItemType::Frame) got.push_back(i.bytes);
        }
        CHECK(back == stream, "iteration %u: bytes not conserved", it);
        CHECK(got == frames, "iteration %u: %zu frames in, %zu out", it, frames.size(), got.size());
        framesIn += unsigned(frames.size());
        framesOut += unsigned(got.size());
    }
    std::printf("fuzz: %u streams, %u frames in / %u frames out, random chunking + noise\n", iters, framesIn, framesOut);

    /* pure random bytes: must never crash, must conserve bytes */
    for (unsigned it = 0; it < 300; ++it) {
        std::vector<uint8_t> s(1 + rng() % 2000);
        for (auto &b : s) b = uint8_t(rng());
        Collector c; StreamParser p(Dir::NcpToHost, c.emit());
        p.feed(s.data(), s.size(), 0); p.flush();
        std::vector<uint8_t> back;
        for (auto &i : c.items) back.insert(back.end(), i.bytes.begin(), i.bytes.end());
        CHECK(back == s, "random stream %u not conserved", it);
    }
}

static void testSpecificCases() {
    std::mt19937 rng(3);
    /* 1) a header that looks right for 1 byte but has an unknown ID: only byte 0 is junk */
    {
        std::vector<uint8_t> s = {0x20, 0x00, 0x77, 0x77, 0x20, 0x02, 0x01, 0x00, 0x00, 0x00};
        Collector c; StreamParser p(Dir::NcpToHost, c.emit());
        p.feed(s.data(), s.size(), 0);
        CHECK(c.items.size() == 2 && c.items[0].bytes.size() == 4, "expected one 4-byte unsynced run + frame, got %zu items", c.items.size());
        std::vector<uint8_t> back; int frames = 0;
        for (auto &i : c.items) { back.insert(back.end(), i.bytes.begin(), i.bytes.end()); frames += i.type == ItemType::Frame; }
        CHECK(back == s && frames == 1 && c.items.back().type == ItemType::Frame, "resync to hello response");
    }
    /* 2) truncation after gap */
    {
        std::vector<uint8_t> s = {0xA0, 0x12, 0x01, 0x00, 1, 2, 3};
        Collector c; StreamParser p(Dir::NcpToHost, c.emit());
        p.feed(s.data(), s.size(), 1000);
        p.poll(1000 + 100000);
        CHECK(c.items.empty(), "should still wait at 100 ms");
        p.poll(1000 + 200000);
        CHECK(c.items.size() == 1 && c.items[0].type == ItemType::Truncated && c.items[0].bytes == s, "truncated item");
        Record r = recordFor(Dir::NcpToHost, c.items[0]);
        CHECK(r.sev == Severity::Error && r.name.find("7 of 22") != std::string::npos, "truncated name %s", r.name.c_str());
    }
    /* 3) generic error response: rsp_connection_open with result only */
    {
        std::vector<uint8_t> s = {0x20, 0x02, 0x06, 0x04, 0x0f, 0x00};
        Collector c; StreamParser p(Dir::NcpToHost, c.emit());
        p.feed(s.data(), s.size(), 0);
        CHECK(c.items.size() == 1 && c.items[0].type == ItemType::Frame, "generic error framed");
        Record r = recordFor(Dir::NcpToHost, c.items[0]);
        CHECK(r.genericError && r.result == 0x000f && r.sev == Severity::Info, "generic error flagged");
    }
    /* 4) variable array whose length field disagrees with the frame length */
    {
        /* cmd_gatt_write_characteristic_value: conn, char(2), len=5 but only 3 data bytes */
        std::vector<uint8_t> s = {0x20, 0x07, 0x09, 0x09, 0x01, 0x10, 0x00, 0x05, 0xaa, 0xbb, 0xcc};
        Record r; r.dir = Dir::HostToNcp; r.bytes = s; analyseFrame(r);
        CHECK(r.sev == Severity::Error && !r.issues.empty(), "array mismatch flagged");
    }
    /* 5) header length field vs bytes typed (Send panel) */
    {
        std::vector<uint8_t> s = {0x20, 0x01, 0x01, 0x00};
        Record r; r.dir = Dir::HostToNcp; r.bytes = s; analyseFrame(r);
        CHECK(r.sev == Severity::Error, "missing payload flagged");
    }
    /* 6) a length > 256 never frames (BGLib would reject it too) */
    {
        std::vector<uint8_t> s = {0xA2, 0x00, 0x01, 0x00};
        Collector c; StreamParser p(Dir::NcpToHost, c.emit());
        p.feed(s.data(), s.size(), 0); p.flush();
        CHECK(!c.items.empty() && c.items[0].type != ItemType::Frame, "len>256 rejected");
    }
    /* 7) host direction: event bit is never a header start */
    {
        std::vector<uint8_t> s = {0xA0, 0x00, 0x01, 0x00};
        Collector c; StreamParser p(Dir::HostToNcp, c.emit());
        p.feed(s.data(), s.size(), 0); p.flush();
        CHECK(c.items.size() == 1 && c.items[0].type == ItemType::Unsynced, "event header from host is unsynced");
    }
    /* 8) extraKnown: response to a command ID that is not in the SDK table */
    {
        std::vector<uint8_t> s = {0x20, 0x02, 0x01, 0x7e, 0x01, 0x01};
        Collector c; StreamParser p(Dir::NcpToHost, c.emit(), [](uint32_t id) { return id == 0x7e010020u; });
        p.feed(s.data(), s.size(), 0);
        CHECK(c.items.size() == 1 && c.items[0].type == ItemType::Frame, "unknown-ID response framed when outstanding");
    }
    (void)rng;
}

int main() {
    testKnownSizes();
    testEveryMessage();
    testConservationFuzz();
    testSpecificCases();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
