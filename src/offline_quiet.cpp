// Offline Quiet: hides the EA sign-in popups F1 25 raises on every modded (offline) boot and answers them
// the way the player would:
//   ea_login_start                    "EA LOGIN / Please wait"      -> "skip" (Cancel and play offline)
//   ea_login_in_progress              "EA LOGIN / Please wait"      -> hidden (spinner only, no button)
//   online_services_EA_not_logged_in  "ONLINE SERVICES ERROR"       -> "ok"
//   no_chat_privilege_msg             "CHAT RESTRICTED"             -> "ok"
//
// Every game popup is built by Ui_Message_Construct (base+0x42d9bf0: system, out handle, id string,
// response callback, 0, 0). For a hidden id the hook returns an empty handle, which the game's attribute
// setter and release accept (Swofty's netplay driver does the same), or a stand-in message for tasks that
// check it, and calls the response callback on the next frame from the frame tick: callback(ctx, &NeStr button).
//
// The constructor is located through the beta_motd poster's call to it (the signature Swofty's core uses),
// and the hook chains onto whatever sits there: the core's own jmp (its popup hook) or the plain prologue.

#include <swofty/events.hpp>
#include <swofty/mixin.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct FixedId {
    const void *vt;
    uint64_t cap;
    char *data;
};

struct Handle {
    void *sys;
    void *msg;
};

struct Callback {
    void *fn;
    void *ctx;
};

using Construct_t = Handle *(void *sys, Handle *out, FixedId *id, Callback *cb, const char *a5, uint64_t a6);
using MakeStr_t = uint32_t(void **out, const char *s, uint32_t flags, uintptr_t len);
using PopupCb_t = void(void *ctx, void **response);

struct Rule {
    const char *id;
    const char *answer; // nullptr: hide only
    const char *after;  // when set: only within AFTER_MS of hiding this id
    int group;          // which setting turns it on: 0 EA login, 1 online error, 2 chat restricted
    bool stand_in;      // hand back a stand-in message instead of an empty handle (see make_stand_in)
};

constexpr Rule RULES[] = {
    // Answered like the player does in the stock game (0.1.4 left it unanswered: after the 5 s timeout the
    // game asked again with ea_login_in_progress every frame, then ea_account_login_error).
    {"ea_login_start", "skip", nullptr, 0, false},
    {"ea_login_in_progress", nullptr, nullptr, 0, false},
    // Its task (error reporter base+0x32c1130) checks the message right after building it and again in the
    // response callback (base+0x32c1ac0): handle.msg->def(+0x10)->name(+0x38) must be a non-empty string.
    // With an empty handle (0.1.3-0.1.6) it raised online_services_generic_exception_online_with_code
    // instead, ignored the "ok", and the stuck task kept Time Trial loading forever.
    {"online_services_EA_not_logged_in", "ok", nullptr, 1, true},
    {"no_chat_privilege_msg", "ok", nullptr, 2, false},
    // Raised by the same task together with online_services_EA_not_logged_in. In the stock game the player's
    // Okay on that one ends the task, which takes this popup with it, so the player never answers it. Left on
    // screen after the task is gone, its response callback reads the freed task (0.1.2 crash, base+0x3418d80).
    {"online_services_generic_exception_online_with_code", nullptr, "online_services_EA_not_logged_in", 1, false},
};
constexpr uint64_t AFTER_MS = 2000;

// One switch per popup in ADDON SETTINGS (swofty.json options), so each can be tried on its own.
SWOFTY_SETTING(bool, hide_ea_login, true);
SWOFTY_SETTING(bool, hide_online_error, true);
SWOFTY_SETTING(bool, hide_chat_restricted, true);

bool group_on(int g)
{
    return g == 0 ? bool(hide_ea_login) : g == 1 ? bool(hide_online_error) : bool(hide_chat_restricted);
}

constexpr uintptr_t RVA_CONSTRUCT = 0x42d9bf0; // build d873a1c8, the fallback when the signature misses
constexpr const char *S_MOTD =
    "48 B9 62 65 74 61 5F 6D 6F 74 48 89 4C 24 58 66 C7 44 24 60 64 00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 40 C5 F9 57 "
    "C0 C5 F9 29 44 24 30 C5 F9 11 44 24 20 48 8D 9C 24 E0 00 00 00 4C 8D 44 24 40 4C 8D 4C 24 30 48 89 C1 48 89 DA "
    "E8 ?? ?? ?? ??";
constexpr uintptr_t MOTD_CONSTRUCT = 0x4b; // offset of the call's rel32 in S_MOTD
constexpr uint8_t PROLOGUE[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x30};
// Answers go on the next frame tick (~20 ms, like 0.1.3), never inside the constructor: the popup's task
// still has to store the handle. (0.1.4-0.1.6 blamed the Time Trial hang on answering too fast and waited;
// the cause was the empty handle, see make_stand_in.)
constexpr uint64_t ANSWER_MS = 0;
constexpr int LOG_IDS = 200;

Construct_t *orig_construct;
MakeStr_t *make_str;
std::atomic<int> seen;
std::atomic<uint64_t> hidden_at[sizeof RULES / sizeof *RULES];

uint64_t hidden_ms(const char *id)
{
    for (size_t i = 0; i < sizeof RULES / sizeof *RULES; i++)
        if (!std::strcmp(RULES[i].id, id))
            return hidden_at[i];
    return 0;
}

struct Pending {
    void *fn, *ctx;
    const char *answer, *id;
    uint64_t due;
};
std::mutex lock;
std::vector<Pending> pending;

bool readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi{};
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return uintptr_t(p) + n <= uintptr_t(mi.BaseAddress) + mi.RegionSize;
}

const char *id_of(const FixedId *id)
{
    if (!readable(id, sizeof *id) || !readable(id->data, 1))
        return nullptr;
    return id->data;
}

// A message object the UI never sees. Only the fields the task and the listener handles touch are set:
//   +0x10 -> definition, whose +0x38 is the message name (what "Callback invalidated: %s" prints)
//   +0x90/+0x98/+0xa0 listener list: begin, end, capacity (entries). Ne_ListenerHandle_Assign/_Unregister
//   (base+0x42aa6c0/0x42aa980) append/remove handles there; the buffer is big enough that they never ask the
//   allocator at +0x88 to grow it. `out` is listed first, like the real constructor registers it.
// Never freed (a few hundred bytes per boot).
constexpr size_t STAND_IN_LISTENERS = 64;
void *make_stand_in(const char *name, Handle *out)
{
    auto *m = static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!m)
        return nullptr;
    uint8_t *def = m + 0x200;
    char *str = reinterpret_cast<char *>(m + 0x300);
    void **list = reinterpret_cast<void **>(m + 0x400);
    std::strncpy(str, name, 0xff);
    *reinterpret_cast<char **>(def + 0x38) = str;
    *reinterpret_cast<uint8_t **>(m + 0x10) = def;
    size_t n = 0;
    if (out)
        list[n++] = out;
    *reinterpret_cast<void ***>(m + 0x90) = list;
    *reinterpret_cast<void ***>(m + 0x98) = list + n;
    *reinterpret_cast<uint64_t *>(m + 0xa0) = STAND_IN_LISTENERS;
    return m;
}

Handle *construct_hook(void *sys, Handle *out, FixedId *id, Callback *cb, const char *a5, uint64_t a6)
{
    const char *name = id_of(id);
    if (name) {
        for (size_t i = 0; i < sizeof RULES / sizeof *RULES; i++) {
            const Rule &r = RULES[i];
            if (std::strncmp(name, r.id, 64) != 0)
                continue;
            if (!group_on(r.group))
                break; // switched off: show it
            uint64_t now = GetTickCount64();
            if (r.after) {
                uint64_t t = hidden_ms(r.after);
                if (!t || now - t > AFTER_MS)
                    break; // not the boot pair: show it
            }
            hidden_at[i] = now;
            void *fn = readable(cb, sizeof *cb) ? cb->fn : nullptr, *ctx = fn ? cb->ctx : nullptr;
            swofty::log("[offline-quiet] hid '%s' (callback %p)%s%s%s", r.id, fn, r.answer ? ", answering '" : "",
                        r.answer ? r.answer : "", r.answer ? (fn ? "'" : "' (no callback, nothing to answer)") : "");
            if (r.answer && fn) {
                std::lock_guard<std::mutex> g(lock);
                pending.push_back({fn, ctx, r.answer, r.id, now + ANSWER_MS});
            }
            if (out)
                *out = Handle{sys, r.stand_in ? make_stand_in(r.id, out) : nullptr};
            return out;
        }
        if (seen.fetch_add(1) < LOG_IDS)
            swofty::log("[offline-quiet] popup '%.64s' shown", name);
    }
    return orig_construct(sys, out, id, cb, a5, a6);
}

void tick(double, void *)
{
    std::vector<Pending> due;
    {
        std::lock_guard<std::mutex> g(lock);
        uint64_t now = GetTickCount64();
        for (auto it = pending.begin(); it != pending.end();) {
            if (now >= it->due) {
                due.push_back(*it);
                it = pending.erase(it);
            } else
                ++it;
        }
    }
    for (const Pending &p : due) {
        void *s = nullptr;
        make_str(&s, p.answer, 0, 0);
        reinterpret_cast<PopupCb_t *>(p.fn)(p.ctx, &s);
        make_str(&s, nullptr, 0, 0);
        swofty::log("[offline-quiet] answered '%s' with '%s'", p.id, p.answer);
    }
}

void put_abs_jmp(uint8_t *at, uintptr_t to)
{
    at[0] = 0xFF;
    at[1] = 0x25;
    std::memset(at + 2, 0, 4);
    std::memcpy(at + 6, &to, 8);
}

// A run of int3 padding between the game's functions after `from`, for the 14-byte gate. It is in the
// game image, so always within rel32 reach. 0.1.1 allocated the gate with VirtualAlloc and got memory 3.8 GB
// away (the rel32 wrapped and the game jumped to garbage).
uint8_t *find_cave(uintptr_t from)
{
    constexpr size_t RUN = 24, SPAN = 16u << 20;
    size_t run = 0;
    for (uintptr_t p = from; p < from + SPAN; p++) {
        if (!readable(reinterpret_cast<void *>(p), 1))
            break;
        run = *reinterpret_cast<const uint8_t *>(p) == 0xCC ? run + 1 : 0;
        if (run == RUN)
            return reinterpret_cast<uint8_t *>(p - RUN + 1 + 4); // keep 4 bytes of padding after the function
    }
    return nullptr;
}

bool reach(uintptr_t from_next, uintptr_t to)
{
    intptr_t d = intptr_t(to) - intptr_t(from_next);
    return d >= INT32_MIN && d <= INT32_MAX;
}

bool hook(uintptr_t target)
{
    auto *code = reinterpret_cast<uint8_t *>(target);
    auto *mem = static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t *gate = find_cave(target);
    if (!mem || !gate || !reach(target + 5, uintptr_t(gate))) {
        swofty::log("[offline-quiet] no room for the hook (trampoline %p, gate %p); popups stay", static_cast<void *>(mem),
                    static_cast<void *>(gate));
        if (mem)
            VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }
    uint8_t *tramp = mem;
    DWORD gold;
    if (!VirtualProtect(gate, 14, PAGE_EXECUTE_READWRITE, &gold))
        return false;
    put_abs_jmp(gate, reinterpret_cast<uintptr_t>(&construct_hook));
    VirtualProtect(gate, 14, gold, &gold);
    FlushInstructionCache(GetCurrentProcess(), gate, 14);
    swofty::log("[offline-quiet] gate at base+0x%llx (int3 padding), trampoline at %p",
                (unsigned long long)(uintptr_t(gate) - swofty::api->module_base()), static_cast<void *>(tramp));
    const char *how;
    if (code[0] == 0xE9) { // another hook (the core's popup hook) already sits here: chain to it
        put_abs_jmp(tramp, target + 5 + *reinterpret_cast<int32_t *>(code + 1));
        how = "after another hook";
    } else if (code[0] == 0xFF && code[1] == 0x25 && *reinterpret_cast<int32_t *>(code + 2) == 0) {
        put_abs_jmp(tramp, *reinterpret_cast<uintptr_t *>(code + 6));
        how = "after another hook (absolute)";
    } else if (!std::memcmp(code, PROLOGUE, sizeof PROLOGUE)) {
        std::memcpy(tramp, PROLOGUE, sizeof PROLOGUE);
        put_abs_jmp(tramp + sizeof PROLOGUE, target + sizeof PROLOGUE);
        how = "on the game's prologue";
    } else {
        swofty::log("[offline-quiet] Ui_Message_Construct starts with %02x %02x %02x %02x %02x, not a known form; "
                    "popups stay",
                    code[0], code[1], code[2], code[3], code[4]);
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }
    orig_construct = reinterpret_cast<Construct_t *>(tramp);
    DWORD old;
    if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    uint8_t jmp[5] = {0xE9};
    int32_t rel = int32_t(reinterpret_cast<intptr_t>(gate) - intptr_t(target + 5));
    std::memcpy(jmp + 1, &rel, 4);
    std::memcpy(code, jmp, 5);
    VirtualProtect(code, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    swofty::log("[offline-quiet] popup hook at base+0x%llx installed %s", (unsigned long long)(target - swofty::api->module_base()),
                how);
    return true;
}

bool init()
{
    uintptr_t base = swofty::api->module_base(), hit = 0, target = 0;
    int n = swofty::api->scan(".xtls", S_MOTD, &hit);
    if (n == 1 && hit)
        target = hit + MOTD_CONSTRUCT + 4 + *reinterpret_cast<const int32_t *>(hit + MOTD_CONSTRUCT);
    else {
        target = base + RVA_CONSTRUCT;
        swofty::log("[offline-quiet] beta_motd signature: %d hit(s); trying base+0x%llx", n,
                    (unsigned long long)RVA_CONSTRUCT);
    }
    make_str = reinterpret_cast<MakeStr_t *>(swofty::api->resolve("Ne_Make_StrFromCStr"));
    if (!make_str) {
        swofty::log("[offline-quiet] Ne_Make_StrFromCStr does not resolve; popups stay");
        return true;
    }
    if (swofty::events::on_frame(tick) != swofty::events::Result::ok)
        swofty::log("[offline-quiet] no frame tick: hidden popups will not be answered");
    swofty::log("[offline-quiet] settings: EA login %s, online error %s, chat restricted %s",
                hide_ea_login ? "hidden" : "shown", hide_online_error ? "hidden" : "shown",
                hide_chat_restricted ? "hidden" : "shown");
    hook(target);
    return true;
}

} // namespace

SWOFTY_ADDON(init)
