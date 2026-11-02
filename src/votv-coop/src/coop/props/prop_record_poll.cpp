// coop/props/prop_record_poll.cpp -- see coop/props/prop_record_poll.h.

#include "coop/props/prop_record_poll.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/prop_save_data.h"
#include "coop/text/utf8_codec.h"         // ToUtf8: the census line
#include "ue_wrap/actors/save_record.h"    // CaptureRecord / OverridesGetData / SaveRecord
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"     // ForEachClass / ForEachInstance
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::prop_record_poll {

namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace WI = ue_wrap::world_identity;

// The pass's shape: one second between passes; at most this many records captured per tick, so a
// world full of matched props spends a bounded slice of a frame on the poll; the cursor rotates
// through the worklist, so nothing starves and a change anywhere lands within a few seconds.
constexpr uint64_t kPassMs       = 1000;
constexpr size_t   kCapturesPerPass = 24;
// The hash map's ceiling: far above the matched props of any world; past it, new keys stop being
// tracked (said once) rather than the map growing with a join's every prop.
constexpr size_t   kMaxTracked   = 4096;
constexpr size_t   kCensusCap    = 64;   // class names said in one census line, counted beyond

// The matched classes and their per-class key offsets, rebuilt when the world changes or the
// pattern row does: a Blueprint class can come back as a new object after a travel, and its
// offsets are re-resolved with it.
struct MatchedClass {
    void*   cls = nullptr;
    std::wstring name;
    int32_t keyOff = -1;      // the class's `Key` FName property; -1 = keyless, skipped
};
uint32_t    g_gen = 0;
std::string g_lastPatterns;   // the row's value the list was built from: a change rebuilds it
std::vector<MatchedClass> g_classes;
std::vector<std::wstring> g_census;
bool        g_censusSaid = false;
size_t      g_censusMore = 0;

// The value hash per prop key. First sight stores; a move publishes.
std::unordered_map<std::wstring, uint64_t> g_hashes;

size_t   g_saidCap = 0;
size_t   s_cursor = 0;
uint64_t s_lastPassMs = 0;

std::atomic<coop::net::Session*> g_session{nullptr};

// The patterns, lower-cased once per world (the row's value is read each pass, so an ini change
// lands without a restart; the class LIST is what stays world-scoped).
std::vector<std::wstring> SplitPatterns(const std::string& csv) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (char c : csv) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c > ' ' && c < 127) cur.push_back(static_cast<wchar_t>(c));
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool NameMatches(const std::wstring& lowerName, const std::vector<std::wstring>& patterns) {
    for (const std::wstring& p : patterns)
        if (lowerName.find(p) != std::wstring::npos) return true;
    return false;
}

std::wstring LowerAscii(const std::wstring& in) {
    std::wstring out = in;
    for (wchar_t& c : out)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return out;
}

// A record's VALUES as one 64-bit FNV-1a, the dev digest's stance in one number: lengths go in
// first so {"ab","c"} and {"a","bc"} do not collide, and the signal rows are skipped entirely --
// a photo must not cost a walk, and no state this lane exists for lives in a signal group.
// Class, key and transform are the map's key and the prop lanes', never the poll's business.
struct Fnv64 {
    uint64_t h = 1469598103934665603ull;
    void Bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    template <class T> void Pod(const T& v) { Bytes(&v, sizeof(v)); }
    void Str(const std::wstring& s) { Pod(static_cast<uint32_t>(s.size())); Bytes(s.data(), s.size() * sizeof(wchar_t)); }
};

uint64_t HashValues(const SR::SaveRecord& r) {
    Fnv64 f;
    auto podGroup = [&f](const auto& vv) {
        for (const auto& v : vv) {
            f.Pod(static_cast<uint32_t>(v.size()));
            for (const auto& e : v) f.Pod(e);
        }
    };
    podGroup(r.bools);
    podGroup(r.floats);
    podGroup(r.ints);
    for (const auto& v : r.strings) for (const auto& e : v) f.Str(e);
    for (const auto& v : r.classes) for (const auto& e : v) f.Str(e);
    podGroup(r.vectors);
    podGroup(r.rotators);
    podGroup(r.transforms);
    podGroup(r.bytes);
    for (const auto& v : r.names) for (const auto& e : v) f.Str(e);
    return f.h;
}

// The class list of one world: every loaded class whose NAME matches a pattern and that carries a
// `Key` FName property to address its instances by. A class without a key is said (once per
// world) and skipped: the record wire addresses by key, and a keyless prop has no identity to
// publish under.
void RebuildClasses(const std::vector<std::wstring>& patterns) {
    g_classes.clear();
    g_census.clear();
    g_censusSaid = false;
    g_censusMore = 0;
    g_hashes.clear();
    struct Ctx { const std::vector<std::wstring>* patterns; } ctx{&patterns};
    ue_wrap::object_index::ForEachClass([](void* c, void* cls, void* /*anyInstance*/) {
        Ctx& cx = *static_cast<Ctx*>(c);
        const std::wstring raw = R::ToString(R::NameOf(cls));
        if (raw.empty()) return;
        const std::wstring lower = LowerAscii(raw);
        if (!NameMatches(lower, *cx.patterns)) {
            // The census: a save-keeping class the patterns do not name. Only classes that CAN
            // keep a record say anything; every other class is none of this lane's business.
            if (SR::OverridesGetData(cls)) {
                if (g_census.size() < kCensusCap) g_census.push_back(raw);
                else ++g_censusMore;
            }
            return;
        }
        MatchedClass m;
        m.cls = cls;
        m.name = raw;
        m.keyOff = R::FindPropertyOffset(cls, L"Key");
        g_classes.push_back(std::move(m));
    }, &ctx);
}

}  // namespace

void SetSession(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (!coop::config::ResolveFlag(coop::config_registry::rows::prop_record_poll)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !s->AnyWorldReadyPeer()) return;

    const uint64_t nowMs = coop::net::NowMs();
    if (s_lastPassMs && nowMs - s_lastPassMs < kPassMs) return;
    s_lastPassMs = nowMs;

    // The patterns are read each pass; the class list is world-scoped AND pattern-scoped: a new
    // world re-resolves the classes, a changed row re-matches them (the census says itself again,
    // and the hash baselines re-take over one quiet pass).
    const uint32_t gen = WI::Generation();
    std::string patternCsv = coop::config::ResolveString(
        coop::config_registry::rows::prop_record_poll_patterns);
    if (gen != g_gen || patternCsv != g_lastPatterns) {
        g_gen = gen;
        g_lastPatterns = patternCsv;
        RebuildClasses(SplitPatterns(patternCsv));
    }

    if (!g_censusSaid && ue_wrap::object_index::IsSeeded() && ue_wrap::object_index::Backlog() == 0) {
        g_censusSaid = true;
        std::string matched, census;
        for (const MatchedClass& m : g_classes) {
            if (!matched.empty()) matched += ", ";
            matched += coop::text::ToUtf8(m.name);
            if (m.keyOff < 0) matched += "(no Key)";
        }
        for (const std::wstring& n : g_census) {
            if (!census.empty()) census += ", ";
            census += coop::text::ToUtf8(n);
        }
        UE_LOGI("prop_record_poll: this world matched %zu class(es) by pattern: [%s]; the "
                "save-keeping classes the patterns do not name: [%s]%s",
                g_classes.size(), matched.c_str(), census.c_str(),
                g_censusMore ? " (more past the cap)" : "");
        if (g_censusMore == 0 && g_census.empty())
            UE_LOGI("prop_record_poll: none -- every save-keeping keyed class is patterned");
    }

    // The worklist of this pass: the live, keyed instances of the matched classes, in class order.
    // Built each pass (the instances are few; the walk is a handful of class-instance lists).
    struct Item { void* actor; std::wstring key; };
    std::vector<Item> list;
    for (const MatchedClass& m : g_classes) {
        if (m.keyOff < 0) continue;
        struct Ctx { const MatchedClass* m; std::vector<Item>* list; } ctx{&m, &list};
        ue_wrap::object_index::ForEachInstance(m.cls, [](void* c, void* obj, int32_t idx) {
            Ctx& cx = *static_cast<Ctx*>(c);
            const auto flags = R::SlotFlags(idx);
            if (flags & R::slot_flags::Dying) return;
            if (flags & R::slot_flags::NotYetReadable) return;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            if (WI::WorldOf(obj) != WI::CurrentWorld()) return;
            const R::FName key = *reinterpret_cast<const R::FName*>(
                reinterpret_cast<const char*>(obj) + cx.m->keyOff);
            const std::wstring ks = R::ToString(key);
            if (ks.empty() || ks == L"None") return;
            cx.list->push_back(Item{ obj, ks });
        }, &ctx);
    }
    if (list.empty()) return;

    const size_t start = s_cursor % list.size();
    size_t taken = 0;
    for (size_t i = 0; i < list.size() && taken < kCapturesPerPass; ++i) {
        const Item& it = list[(start + i) % list.size()];
        ++s_cursor;
        ++taken;
        if (g_hashes.size() >= kMaxTracked && g_hashes.find(it.key) == g_hashes.end()) {
            if (!g_saidCap++) UE_LOGW("prop_record_poll: %zu keys tracked -- new keys stop being "
                                      "tracked (said once)", kMaxTracked);
            continue;
        }
        SR::SaveRecord rec;
        if (!SR::CaptureRecord(it.actor, rec)) continue;   // unreadable this pass; the next retries
        const uint64_t h = HashValues(rec);
        const auto ht = g_hashes.find(it.key);
        if (ht == g_hashes.end()) {
            if (g_hashes.size() < kMaxTracked) g_hashes[it.key] = h;   // first sight: baseline only
            continue;
        }
        if (ht->second == h) continue;
        ht->second = h;
        // The same send the birth paths use: the host broadcasts, a client claims and the host
        // validates, applies and re-publishes canonically. A refused send is the transport's; the
        // next pass re-reads the same hash and asks again.
        if (coop::prop_save_data::Publish(s, it.actor, it.key)) {
            static unsigned long long s_published = 0;
            ++s_published;
            if (s_published == 1 || s_published % 50 == 0)
                UE_LOGI("prop_record_poll: published a moved record (key='%ls', #%llu this session)",
                        it.key.c_str(), s_published);
        }
    }
}

void OnDisconnect() {
    g_hashes.clear();
    s_cursor = 0;
    s_lastPassMs = 0;
    g_saidCap = 0;
    g_session.store(nullptr, std::memory_order_release);
    // The class list and the census are world- and pattern-scoped; the generation and pattern
    // judges above rebuild them when either changes. A reconnect in the same world reuses them.
}

}  // namespace coop::prop_record_poll
