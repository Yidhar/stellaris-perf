// Offline test of perf_tooltip (no game): a stand-in for the provider calls of CTooltipManager::Update is compiled here,
// its two call instructions are found by their bytes and hooked the way the plugin hooks the engine's, and the cache is
// driven with a clock and an input state the test steps. Exit code = failed checks.
//
//   build\Release\tooltip_test.exe
#include "perf_tooltip.hpp"
#include "perf_engine.hpp"
#include "perf_phase.hpp"
#include "perf_tweaks.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

// What perf_tooltip.cpp takes from the rest of the plugin.
namespace perf::phase {
int g_test_gui_depth = 0;
GuiContext::GuiContext() { ++g_test_gui_depth; }
GuiContext::~GuiContext() { --g_test_gui_depth; }
}  // namespace perf::phase
namespace perf {
uint64_t g_test_generation = 1;
uint64_t Generation() { return g_test_generation; }
void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  [log] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
}  // namespace perf

namespace {

int g_failed = 0, g_checked = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        ++g_checked;                                                   \
        if (!(cond)) {                                                 \
            ++g_failed;                                                \
            printf("FAILED line %d: %s\n", __LINE__, #cond);          \
        }                                                              \
    } while (0)

// ---- the stand-in engine ------------------------------------------------------------------------------------------

struct Tip {  // CToolTip: four CStrings, two ints, two flags
    perf::EngineString text[4];
    int32_t x = 0, y = 0;
    bool a = true, b = false;
    uint8_t pad[6] = {};
};
static_assert(sizeof(Tip) == sdk::rt::CToolTip_size, "the stand-in tooltip is not the SDK's size");
static_assert(sdk::rt::CToolTip_string_count == 4 && sdk::rt::CToolTip_state == offsetof(Tip, x), "the stand-in tooltip's layout");

// Real virtual calls, so that the compiler emits the engine's `mov rax, [rcx]; call qword ptr [rax + slot*8]`.
#define V4(n) virtual void n##0() {} virtual void n##1() {} virtual void n##2() {} virtual void n##3() {}
#define V16(n) V4(n##a) V4(n##b) V4(n##c) V4(n##d)

struct Handler {
    virtual bool GetToolTip(void* object, void* tip) = 0;  // slot 0
};
struct GuiObject {
    V16(p) V16(q) V16(r) V16(s) V4(t) virtual void u() {}  // 69 slots ahead of the getter
    virtual void* GetToolTipHandler() = 0;
};
struct Idler {
    V16(p) V16(q)  // 32 slots ahead
    virtual bool BuildToolTip(void* object, void* tip) = 0;
};

struct Text {  // what a provider writes
    int calls = 0;
    bool result = true;
    bool throws = false;
    bool changing = false;  // the text carries the call count
    std::string text;
    bool Write(void* tip) {
        ++calls;
        if (throws) throw 11;
        Tip* t = (Tip*)tip;
        t->text[0].text += text + (changing ? std::to_string(calls) : std::string());
        t->text[1].text = "later";
        t->text[3].text = std::string(300, 'x') + text;  // beyond the small buffer
        t->x += 12;
        t->y = -3;
        t->b = true;
        return result;
    }
};
struct TestHandler : Handler, Text {
    bool GetToolTip(void*, void* tip) override { return Write(tip); }
};
struct TestIdler : Idler, Text {
    bool BuildToolTip(void*, void* tip) override { return Write(tip); }
};
bool g_getter_throws = false;
int g_getter_calls = 0;
struct TestObject : GuiObject {
    Handler* handler = nullptr;
    explicit TestObject(Handler* h) : handler(h) {}
    void* GetToolTipHandler() override {
        ++g_getter_calls;
        if (g_getter_throws) throw 7;
        return handler;
    }
};
struct OtherObject : TestObject {  // another class with the same getter
    using TestObject::TestObject;
    void u() override {}
};
volatile int g_last = 0;

// The part of CTooltipManager::Update the plugin hooks: reset, the object's handler, else the idler.
__declspec(noinline) bool FakeUpdate(GuiObject* object, Idler* idler, Tip* tip) {
    for (perf::EngineString& s : tip->text) s.text.clear();
    tip->x = tip->y = 0;
    tip->a = true;
    tip->b = false;
    Handler* handler = object ? (Handler*)object->GetToolTipHandler() : nullptr;
    if (handler && handler->GetToolTip(object, tip)) {
        g_last = 1;
        return true;
    }
    const bool result = idler->BuildToolTip(object, tip);
    g_last = result ? 2 : 3;
    return result;
}

uint8_t* FindCall(uint8_t* from, size_t slot) {
    const uint32_t disp = (uint32_t)(slot * sizeof(void*));
    for (uint8_t* p = from; p < from + 0x400; ++p) {
        if (p[0] == 0xFF && p[1] == 0x90 && memcmp(p + 2, &disp, 4) == 0) return p;
    }
    return nullptr;
}

void* FakeAssign(void* cstring, const char* chars, size_t length) {
    ((perf::EngineString*)cstring)->text.assign(chars, length);
    return cstring;
}

// ---- the clock and the input state --------------------------------------------------------------------------------

unsigned long g_input = 1000;
long g_cx = 400, g_cy = 300;
uint64_t g_now = 100000;
bool g_sample_ok = true;
bool Sample(unsigned long* last_input, long* cx, long* cy, uint64_t* now) {
    if (!g_sample_ok) return false;
    *last_input = g_input;
    *cx = g_cx;
    *cy = g_cy;
    *now = g_now;
    return true;
}

bool Run(GuiObject* object, Idler* idler, Tip* tip, int* thrown = nullptr) {
    g_now += 16;  // a frame
    try {
        return FakeUpdate(object, idler, tip);
    } catch (int e) {
        if (thrown) *thrown = e;
        return false;
    }
}

}  // namespace

int main() {
    using namespace perf;
    TestHandler handler, other_handler;
    TestIdler idler;
    handler.text = "from the handler";
    other_handler.text = "another handler";
    idler.text = "from the idler";
    TestObject object(&handler), bare(nullptr);
    Tip tip;
    const std::string long_handler = std::string(300, 'x') + "from the handler", long_idler = std::string(300, 'x') + "from the idler";

    // 0. the stand-in works on its own
    CHECK(Run(&object, &idler, &tip) && handler.calls == 1 && idler.calls == 0 && tip.text[0].text == "from the handler");
    CHECK(Run(&bare, &idler, &tip) && idler.calls == 1 && tip.text[0].text == "from the idler" && tip.x == 12);

    uint8_t* code = (uint8_t*)&FakeUpdate;
    if (code[0] == 0xE9) code += 5 + *(int32_t*)(code + 1);  // an incremental-link thunk
    uint8_t* site_a = FindCall(code, sdk::vt::CGuiObject_GetToolTipHandler);
    uint8_t* site_b = FindCall(code, sdk::vt::CGameIdler_BuildToolTip);
    printf("stand-in update at %p, handler getter call at %p, idler call at %p\n", (void*)code, (void*)site_a, (void*)site_b);
    if (!site_a || !site_b) {
        printf("FAILED: the compiler did not emit `call qword ptr [rax + disp32]` for the stand-in's calls\n");
        return 100;
    }
    uint8_t before_a[6], before_b[6];
    memcpy(before_a, site_a, 6);
    memcpy(before_b, site_b, 6);

    CHECK(MH_Initialize() == MH_OK);
    tooltip::detail::SetSampler(&Sample);
    CHECK(tooltip::detail::InstallAt(site_a, site_b, (void*)&FakeAssign));
    CHECK(tooltip::Installed());
    CHECK(site_a[0] == 0xE9 && site_b[0] == 0xE9);
    tooltip::SetMaxAgeMs(250);

    // 1. mode 0: every frame asks, as without the hooks
    tooltip::SetMode(0);
    handler.calls = idler.calls = 0;
    for (int i = 0; i < 3; ++i) CHECK(Run(&object, &idler, &tip) && tip.text[0].text == "from the handler");
    CHECK(handler.calls == 3 && idler.calls == 0 && tooltip::Read().kept == 0);

    // 2. mode 1: built once, then kept for max age; the content and the result are the provider's
    tooltip::SetMode(1);
    handler.calls = 0;
    CHECK(Run(&object, &idler, &tip) && handler.calls == 1);
    for (int i = 0; i < 5; ++i) {
        memset(tip.pad, 0, sizeof tip.pad);
        CHECK(Run(&object, &idler, &tip));
        CHECK(tip.text[0].text == "from the handler" && tip.text[1].text == "later" && tip.text[2].text.empty() &&
              tip.text[3].text == long_handler && tip.x == 12 && tip.y == -3 && tip.a && tip.b);
    }
    CHECK(handler.calls == 1 && idler.calls == 0 && tooltip::Read().kept == 5);
    g_now += 300;  // past max age
    CHECK(Run(&object, &idler, &tip) && handler.calls == 2);
    CHECK(Run(&object, &idler, &tip) && handler.calls == 2);

    // 3. an input: nothing is kept until it is 100 ms old, then the next answer is built once more and kept
    g_input = (unsigned long)g_now;
    for (int i = 0; i < 3; ++i) CHECK(Run(&object, &idler, &tip));
    CHECK(handler.calls == 5);
    g_now += 120;
    CHECK(Run(&object, &idler, &tip) && handler.calls == 6);
    CHECK(Run(&object, &idler, &tip) && handler.calls == 6);

    // 4. the cursor, the generation, the object, its class and its handler are part of the question
    g_cx += 1;
    CHECK(Run(&object, &idler, &tip) && handler.calls == 7);
    CHECK(Run(&object, &idler, &tip) && handler.calls == 7);
    ++perf::g_test_generation;
    CHECK(Run(&object, &idler, &tip) && handler.calls == 8);
    alignas(16) static char storage[sizeof(OtherObject)];
    TestObject* twin = new (storage) TestObject(&handler);
    CHECK(Run(twin, &idler, &tip) && handler.calls == 9);
    CHECK(Run(twin, &idler, &tip) && handler.calls == 9);
    twin = new (storage) OtherObject(&handler);  // the same address, another class
    CHECK(Run(twin, &idler, &tip) && handler.calls == 10);
    twin->handler = &other_handler;
    CHECK(Run(twin, &idler, &tip) && other_handler.calls == 1 && tip.text[0].text == "another handler");
    CHECK(Run(twin, &idler, &tip) && other_handler.calls == 1 && tip.text[0].text == "another handler");

    // 5. a handler without a tooltip: the idler is asked, and both answers are kept (the handler's leftovers too)
    handler.result = false;
    handler.calls = idler.calls = 0;
    CHECK(Run(&object, &idler, &tip) && handler.calls == 1 && idler.calls == 1);
    const std::string both = "from the handlerfrom the idler";
    CHECK(tip.text[0].text == both && tip.x == 24);
    for (int i = 0; i < 4; ++i) CHECK(Run(&object, &idler, &tip) && tip.text[0].text == both && tip.text[3].text == long_idler && tip.x == 24);
    CHECK(handler.calls == 1 && idler.calls == 1);
    handler.result = true;

    // 6. no handler, and no object at all: only the idler
    idler.calls = 0;
    const int getters = g_getter_calls;
    CHECK(Run(&bare, &idler, &tip) && idler.calls == 1 && tip.text[0].text == "from the idler");
    CHECK(Run(&bare, &idler, &tip) && idler.calls == 1 && tip.text[0].text == "from the idler" && tip.x == 12);
    CHECK(g_getter_calls == getters + 2 && g_last == 2);  // the getter itself is always called
    CHECK(Run(nullptr, &idler, &tip) && idler.calls == 2);
    CHECK(Run(nullptr, &idler, &tip) && idler.calls == 2 && tip.text[3].text == long_idler);
    idler.result = false;  // "no tooltip" is an answer too
    g_now += 300;
    CHECK(!Run(nullptr, &idler, &tip) && idler.calls == 3);
    CHECK(!Run(nullptr, &idler, &tip) && idler.calls == 3);
    idler.result = true;

    // 7. verify: asked every frame; an answer that changed counts once per change
    tooltip::SetMode(2);
    g_now += 300;
    handler.calls = 0;
    const tooltip::Counters v0 = tooltip::Read();
    for (int i = 0; i < 4; ++i) CHECK(Run(&object, &idler, &tip));
    CHECK(handler.calls == 4 && tooltip::Read().mismatches == v0.mismatches && tooltip::Read().kept == v0.kept + 3);
    handler.changing = true;
    for (int i = 0; i < 3; ++i) CHECK(Run(&object, &idler, &tip));
    CHECK(handler.calls == 7 && tooltip::Read().mismatches == v0.mismatches + 3);
    handler.changing = false;
    tooltip::SetMode(1);

    // 8. no input state: asked, nothing kept
    g_now += 300;
    g_sample_ok = false;
    handler.calls = 0;
    const uint64_t bypassed = tooltip::Read().bypassed;
    for (int i = 0; i < 3; ++i) CHECK(Run(&object, &idler, &tip));
    CHECK(handler.calls == 3 && tooltip::Read().bypassed == bypassed + 3);
    g_sample_ok = true;

    // 9. exceptions pass through the hooked calls to the caller's handler, and the hooks keep working
    int thrown = 0;
    g_getter_throws = true;
    CHECK(!Run(&object, &idler, &tip, &thrown) && thrown == 7);
    g_getter_throws = false;
    g_now += 300;
    handler.throws = true;
    thrown = 0;
    CHECK(!Run(&object, &idler, &tip, &thrown) && thrown == 11);
    handler.throws = false;
    idler.throws = true;
    thrown = 0;
    g_now += 300;
    CHECK(!Run(&bare, &idler, &tip, &thrown) && thrown == 11);
    idler.throws = false;
    handler.calls = 0;
    CHECK(Run(&object, &idler, &tip) && handler.calls == 1 && tip.text[0].text == "from the handler");
    CHECK(Run(&object, &idler, &tip) && handler.calls == 1 && tip.text[0].text == "from the handler");

    // 10. the hooks removed: the original bytes are back and every frame asks
    CHECK(MH_DisableHook(MH_ALL_HOOKS) == MH_OK);
    CHECK(memcmp(before_a, site_a, 6) == 0 && memcmp(before_b, site_b, 6) == 0);
    handler.calls = 0;
    for (int i = 0; i < 3; ++i) CHECK(Run(&object, &idler, &tip) && tip.text[0].text == "from the handler");
    CHECK(handler.calls == 3);
    MH_Uninitialize();

    printf("%s\n", tooltip::Stats(1e-6).c_str());
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed;
}
