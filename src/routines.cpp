#include "routines.h"

#include <windows.h>

#include <cstdio>

#include "exostring.h"
#include "gameobj.h"
#include "glhook.h"
#include "log.h"
#include "movement.h"
#include "fov.h"
#include "camera.h"
#include "spawner.h"
#include "bridge.h"
#include "lk2_mailbox.h"
#include "npcvariety.h"
#include "offsets.h"
#include "vm.h"
#include "vmstack.h"

namespace k2se {
namespace routines {
namespace {

// The stack accessors now live in vmstack.h, which also owns
// K2SE_ENABLE_STACK_ABI. Track B roughly doubled their number and added the
// CExoString lifetime rules, which is more than an anonymous namespace in the
// routine table should be carrying.
using vmstack::PopFloat;
using vmstack::PopInt;
using vmstack::PopObject;
using vmstack::PopString;
using vmstack::PushFloat;
using vmstack::PushInt;
using vmstack::PushObject;
using vmstack::PushString;

bool g_probeSeen = false;
bool g_extendedSeen = false;
bool g_selfTestSeen = false;
bool g_echoSeen = false;
bool g_bannerPosted = false;
uint32_t g_reportSeenMask = 0;  // test id 0..31 has been reported at least once
uint32_t g_reportPassMask = 0;  // ... and its last REPORTED answer was a pass

// Per-routine dispatch counts, indexed by position in kExtended (not by routine
// id). Only the trace thinning reads them; nothing may make a decision from one.
uint32_t g_dispatchCount[64] = {0};

// --- on-screen banner --------------------------------------------------------
// void __cdecl AurPostString(const char* text, int x, int y, float fLife)
// Verified by disassembly (see offsets.h). The text buffer is static because
// whether the engine copies the string or keeps the pointer is unproven --
// a static buffer is safe under either semantics.
using AurPostStringFn = void(__cdecl*)(const char* text, int x, int y, float fLife);

void PostBanner(int extendedCount) {
    if (g_bannerPosted) return;
    g_bannerPosted = true;
    static char banner[128];
    _snprintf(banner, sizeof(banner), "K2SE %d.%d.%d active - %d extended routines online",
              K2SE_VERSION_MAJOR, K2SE_VERSION_MINOR, K2SE_VERSION_PATCH, extendedCount);
    banner[sizeof(banner) - 1] = '\0';
    auto fn = reinterpret_cast<AurPostStringFn>(off::kAurPostString);
    fn(banner, 5, 5, 10.0f);
    log::Writef("banner posted on screen: \"%s\"", banner);
}

// --- the extended routine registry ------------------------------------------
// IDs are allocated from 877 upward and, once shipped in a release, are a
// PUBLIC CONTRACT: never reuse an ID. Keep this table, nss/k2se.nss and the
// header generator in tools/m4_deploy_test.py in lockstep -- nwnnsscomp
// assigns IDs by POSITION in nwscript.nss, so table order here must equal
// prototype order there.
//
// 878/879 are development-only self-test routines; they may be renumbered
// before the first public release (nothing has shipped yet).

using ExtendedHandler = int (*)(int nParams);

// 877: int K2SE_GetVersion()
int H_GetVersion(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(kVersionEncoded)) return off::kErrPushFailed;
#endif
    return 0;
}

// 878: int K2SE_SelfTest(int nFirst, float fSecond, int nThird)
// End-to-end check of the multi-argument ABI. The return value is
// order-sensitive on purpose: if arguments popped in any order other than
// declaration order, the checksum comes out different and the script-side
// comparison fails loudly.
int H_SelfTest(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int nFirst = 0, nThird = 0;
    float fSecond = 0.0f;
    if (!PopInt(&nFirst)) return off::kErrParam;
    if (!PopFloat(&fSecond)) return off::kErrParam;
    if (!PopInt(&nThird)) return off::kErrParam;

    const int checksum =
        nFirst * 1000000 + static_cast<int>(fSecond * 10.0f) * 10000 + nThird;

    if (!g_selfTestSeen) {
        g_selfTestSeen = true;
        log::Writef("SelfTest pop order: first=%d  second(float x10)=%d  third=%d  -> %d",
                    nFirst, static_cast<int>(fSecond * 10.0f), nThird, checksum);
    }
    if (!PushInt(checksum)) return off::kErrPushFailed;
#endif
    return 0;
}

// 879: int K2SE_ReportTest(int nTestId, int nExpected, int nActual)
// Lets a compiled test script report what IT observed into k2se.log; the
// script is the only place the full round trip (compiler -> bytecode ->
// interpreter -> K2SE -> back) is visible. Returns 1 on match, 0 otherwise.
int H_ReportTest(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int nTestId = 0, nExpected = 0, nActual = 0;
    if (!PopInt(&nTestId)) return off::kErrParam;
    if (!PopInt(&nExpected)) return off::kErrParam;
    if (!PopInt(&nActual)) return off::kErrParam;

    const bool pass = (nExpected == nActual);

    // One line per test id was the right instinct -- the battery rides a
    // heartbeat and re-runs every few seconds -- but reporting only the FIRST
    // sample made that sample the session's verdict. On 2026-08-29 the first
    // heartbeat fired before the PC could be resolved, five tests logged FAIL,
    // and the routines then worked for the rest of the session with nothing in
    // the log to say so. The on-screen banner disagreed with the log for two
    // hours and only a human looking at the screen could tell.
    //
    // So: still one line per id in the steady state, plus a line whenever the
    // ANSWER CHANGES. A stable session logs byte-for-byte what it logged
    // before; an unstable one becomes self-describing.
    const bool inRange = nTestId >= 0 && nTestId < 32;
    const uint32_t bit = inRange ? (1u << nTestId) : 0u;
    const bool firstTime = inRange && !(g_reportSeenMask & bit);
    const bool changed = inRange && !firstTime && (pass != ((g_reportPassMask & bit) != 0));

    if (firstTime || changed) {
        g_reportSeenMask |= bit;
        if (pass)
            g_reportPassMask |= bit;
        else
            g_reportPassMask &= ~bit;

        if (changed)
            log::Writef("TEST %2d: expected %d, got %d  ->  %s   "
                        "(CHANGED at t+%u ms -- it answered %s before)",
                        nTestId, nExpected, nActual, pass ? "PASS" : "FAIL",
                        log::MillisSinceInit(), pass ? "FAIL" : "PASS");
        else
            log::Writef("TEST %2d: expected %d, got %d  ->  %s", nTestId, nExpected,
                        nActual, pass ? "PASS" : "FAIL");
    } else {
        log::Trace("test %d: expected %d, got %d -> %s", nTestId, nExpected, nActual,
                   pass ? "pass" : "FAIL");
    }
    if (!PushInt(pass ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}

// 880: string K2SE_EchoString(string sIn)
// The string round trip, end to end: pop a script-supplied string, then push it
// straight back. A script comparing the result against what it sent proves the
// whole path -- compiler, bytecode, VM stack, CExoString construction, and the
// engine's copy semantics in both directions.
//
// It is deliberately an identity function. Anything cleverer would make a
// failure ambiguous between "the string ABI is wrong" and "the transformation is
// wrong", and the string ABI is the thing under test.
int H_EchoString(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    // Must be constructed before the engine touches it: PopString copy-assigns,
    // which frees whatever pointer it finds in the destination first.
    ExoString value;
    if (!value.valid()) {
        log::Write("EchoString: CExoString construction failed");
        return off::kErrParam;
    }
    if (!PopString(&value)) return off::kErrParam;

    if (!g_echoSeen) {
        g_echoSeen = true;
        // Both numbers, because their DIFFERENCE is the finding: the engine's
        // second field counts the allocation, not the characters. For a fresh
        // string they differ by exactly one; after a buffer-reusing assignment
        // they differ by more, which is why length() scans rather than subtracts.
        log::Writef("*** FIRST STRING ROUND TRIP: received \"%s\" "
                    "(length %u, buffer %u) ***",
                    value.c_str(), value.length(), value.buffer_size());
    }

    // The engine allocates its own copy here, so `value` stays ours and its
    // destructor still runs on the way out of this scope.
    if (!PushString(&value)) return off::kErrPushFailed;
#endif
    return 0;
}

// --- the first routines meant for actual mods --------------------------------
// 881..884. Everything before these was plumbing that only proves K2SE works.
//
// All four are READ-ONLY, deliberately. The struct offsets they walk are the
// least-verified thing in the project -- imported layout claims that cannot be
// checked against the file, only by being right. A wrong offset on a read costs
// a wrong number; the same offset on a write corrupts a creature in the player's
// save. Mutators (SetSkillRank, AddFeat, the attribute setters) all have verified
// addresses and confirmed signatures, and are deliberately held back until this
// read path has been confirmed in a live session.
//
// Every one returns a sentinel rather than failing the script, so a mod can test
// the result instead of dying:
//   -1  the creature, its stats, or the read could not be resolved

constexpr int kUnknown = -1;

// Shared prologue: pop an object id, resolve it to a creature's stats block.
// Returns null and leaves *popOk false if the argument itself could not be read,
// which is the one case where we must not push a result.
// `who` is the caller's name, and it is not decoration: all four routines share
// this path, so without it the log cannot say which one was asking.
void* PopCreatureStats(const char* who, bool* popOk) {
    *popOk = false;
#if K2SE_ENABLE_STACK_ABI
    // PopObject, not PopInt. The VM stack is typed -- the pop path checks the
    // slot's type tag before handing the value over (StackPopString, for
    // instance, tests for tag 5) -- so popping a declared `object` as an int
    // would simply fail. An OBJECT is a uint32 handle, but the accessors are not
    // interchangeable.
    uint32_t objectId = 0;
    if (!PopObject(&objectId)) return nullptr;
    *popOk = true;
    void* creature = gameobj::CreatureFromObjectId(objectId);
    void* stats = creature ? gameobj::CreatureStats(creature) : nullptr;
    gameobj::ReportWalk(who);
    return stats;
#else
    (void)who;
    return nullptr;
#endif
}

// 881: int K2SE_GetAbilityScoreBase(object oCreature, int nAbility)
// The base attribute, before items and effects. Reads a byte out of the stats
// block; makes no engine call at all, which makes it the safest of the four.
int H_GetAbilityScoreBase(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    bool popped = false;
    void* stats = PopCreatureStats("K2SE_GetAbilityScoreBase", &popped);
    if (!popped) return off::kErrParam;

    int ability = 0;
    if (!PopInt(&ability)) return off::kErrParam;

    int value = kUnknown;
    if (stats) gameobj::AbilityBase(stats, ability, &value);
    if (!PushInt(value)) return off::kErrPushFailed;
#endif
    return 0;
}

// 882: int K2SE_GetSkillRankBase(object oCreature, int nSkill)
// The rank the creature actually bought, without item or effect modifiers --
// which vanilla GetSkillRank cannot report.
int H_GetSkillRankBase(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    bool popped = false;
    void* stats = PopCreatureStats("K2SE_GetSkillRankBase", &popped);
    if (!popped) return off::kErrParam;

    int skill = 0;
    if (!PopInt(&skill)) return off::kErrParam;

    int value = kUnknown;
    if (stats) gameobj::SkillRankBase(stats, skill, &value);
    if (!PushInt(value)) return off::kErrPushFailed;
#endif
    return 0;
}

// 883: int K2SE_GetFeatAcquired(object oCreature, int nFeat)
// Whether the creature HAS the feat, as opposed to whether it can use it right
// now -- the distinction vanilla GetHasFeat blurs by folding in daily uses.
int H_GetFeatAcquired(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    bool popped = false;
    void* stats = PopCreatureStats("K2SE_GetFeatAcquired", &popped);
    if (!popped) return off::kErrParam;

    int feat = 0;
    if (!PopInt(&feat)) return off::kErrParam;

    int value = kUnknown;
    if (stats) gameobj::HasFeat(stats, feat, &value);
    if (!PushInt(value)) return off::kErrPushFailed;
#endif
    return 0;
}

// 884: int K2SE_GetSpellAcquired(object oCreature, int nSpell)
// Known, regardless of whether there are Force points to cast it.
int H_GetSpellAcquired(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    bool popped = false;
    void* stats = PopCreatureStats("K2SE_GetSpellAcquired", &popped);
    if (!popped) return off::kErrParam;

    int spell = 0;
    if (!PopInt(&spell)) return off::kErrParam;

    int value = kUnknown;
    if (stats) gameobj::HasSpell(stats, spell, &value);
    if (!PushInt(value)) return off::kErrPushFailed;
#endif
    return 0;
}

// --- runtime fog (M5) --------------------------------------------------------
// 885..888. These control a GL-level override, not a per-area game property.
//
// DESIGN.md M5 originally sketched K2SE_SetAreaFog(oArea, ...). That shape would
// be a lie about what actually happens: the fix works by owning the fog values
// the engine hands to OpenGL, which is global to the frame. Naming it after
// areas would promise per-area state that does not exist. M5 also concluded that
// per-room fog is not a thing on this engine and that density is never read,
// which is why neither appears here.
//
// All four are inert unless fog support is installed (k2se_fog.txt next to the
// exe), so a mod that calls them on a machine without it degrades quietly.

// 885: int K2SE_SetFogEnabled(int bEnable)
int H_SetFogEnabled(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int enable = 0;
    if (!PopInt(&enable)) return off::kErrParam;
    glhook::SetEnabled(enable != 0);
    if (!PushInt(glhook::Status())) return off::kErrPushFailed;
#endif
    return 0;
}

// 886: int K2SE_SetFogRange(float fStart, float fEnd)
// Distances in the engine's world units. Linear fog only -- the ARB option K2SE
// injects is ARB_fog_linear, and this engine's fog was always linear.
int H_SetFogRange(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    float start = 0.0f, end = 0.0f;
    if (!PopFloat(&start)) return off::kErrParam;
    if (!PopFloat(&end)) return off::kErrParam;
    glhook::SetRange(start, end);
    if (!PushInt(glhook::Status())) return off::kErrPushFailed;
#endif
    return 0;
}

// 887: int K2SE_SetFogColor(float fRed, float fGreen, float fBlue)
// Components are 0.0..1.0, matching GL rather than the packed 0xRRGGBB integer
// the M5 sketch used -- floats are what actually reaches glFogfv, and converting
// would only lose precision.
int H_SetFogColor(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (!PopFloat(&r)) return off::kErrParam;
    if (!PopFloat(&g)) return off::kErrParam;
    if (!PopFloat(&b)) return off::kErrParam;
    glhook::SetColor(r, g, b);
    if (!PushInt(glhook::Status())) return off::kErrPushFailed;
#endif
    return 0;
}

// 888: int K2SE_GetFogStatus()
// Bit flags: 1 installed, 2 a fragment program was rewritten, 4 override active,
// 8 refused. 0 means fog support is off. Lets a mod explain itself to the player
// instead of silently doing nothing.
int H_GetFogStatus(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(glhook::Status())) return off::kErrPushFailed;
#endif
    return 0;
}

// --- movement (K2 Jump / Crouch / Sprint) -------------------------------------
// 889..893. Thin shells over movement.cpp; the features themselves run on the
// player controller's frame, these only expose state and requests to scripts.

// 889: int K2SE_GetMovementStatus()
// Bit flags (movement.h StatusBits): 1 installed, 2 sprinting, 4 crouching,
// 8 airborne, 16 rolling, 32/64/128/256 sprint/crouch/jump/roll enabled.
int H_GetMovementStatus(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(movement::Status())) return off::kErrPushFailed;
#endif
    return 0;
}

// 890: int K2SE_SetSprintFactor(float fFactor)
// Overrides the ini factor for this session (<= 0 restores the ini value).
int H_SetSprintFactor(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    float factor = 0.0f;
    if (!PopFloat(&factor)) return off::kErrParam;
    movement::SetSprintFactor(factor);
    if (!PushInt(movement::Status())) return off::kErrPushFailed;
#endif
    return 0;
}

// 891: int K2SE_SetCrouch(int bOn)
int H_SetCrouch(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int on = 0;
    if (!PopInt(&on)) return off::kErrParam;
    if (!PushInt(movement::SetCrouch(on != 0) ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}

// 892: int K2SE_Jump()
int H_Jump(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(movement::RequestJump() ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}

// 893: int K2SE_Roll()
int H_Roll(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(movement::RequestRoll() ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}

// 894: int K2SE_SetFOV(float fDegrees, float fSeconds)
// Vertical FOV override for the scene camera (k2-multi-fov); fDegrees <= 0.0
// clears it, fSeconds <= 0.0 keeps the ini smoothing. Only acts in gameplay
// frames, never on dialogue or GUI cameras. TRUE if the FOV module is installed.
int H_SetFOV(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    float degrees = 0.0f, seconds = 0.0f;
    if (!PopFloat(&degrees)) return off::kErrParam;
    if (!PopFloat(&seconds)) return off::kErrParam;
    if (!PushInt(fov::SetOverride(degrees, seconds) ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}

// 895: float K2SE_GetFOV()
int H_GetFOV(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushFloat(fov::Current())) return off::kErrPushFailed;
#endif
    return 0;
}

// 896: int K2SE_SetCameraView(int nView)   0 game style, 1 near, 2 far, 3 first person
int H_SetCameraView(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int view = 0;
    if (!PopInt(&view)) return off::kErrParam;
    if (!PushInt(camera::SetView(view) ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}

// 897: int K2SE_GetCameraView()   -1 when camera views are not installed
int H_GetCameraView(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(camera::GetView())) return off::kErrPushFailed;
#endif
    return 0;
}

// --- spawner, routines 898..907: the data side of k2se_spawn.nss ---------------
// 898: int K2SE_SpawnBegin(string sModule, string sAreaTag)  -> entry count
int H_SpawnBegin(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    ExoString module, area;
    if (!module.valid() || !area.valid()) return off::kErrParam;
    if (!PopString(&module)) return off::kErrParam;
    if (!PopString(&area)) return off::kErrParam;
    if (!PushInt(spawner::Begin(module.c_str(), area.c_str()))) return off::kErrPushFailed;
#endif
    return 0;
}

int PopIndexPushInt(int (*fn)(int)) {
    int index = 0;
    if (!PopInt(&index)) return off::kErrParam;
    if (!PushInt(fn(index))) return off::kErrPushFailed;
    return 0;
}
int PopIndexPushFloat(float (*fn)(int)) {
    int index = 0;
    if (!PopInt(&index)) return off::kErrParam;
    if (!PushFloat(fn(index))) return off::kErrPushFailed;
    return 0;
}
int MarkPresentInt(int i) { return spawner::MarkPresent(i) ? 1 : 0; }
int NeededInt(int i) { return spawner::Needed(i) ? 1 : 0; }

// 899: int K2SE_SpawnMarkPresent(int nIndex)
int H_SpawnMarkPresent(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    return PopIndexPushInt(&MarkPresentInt);
#else
    return 0;
#endif
}
// 900: int K2SE_SpawnNeeded(int nIndex)
int H_SpawnNeeded(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    return PopIndexPushInt(&NeededInt);
#else
    return 0;
#endif
}
// 901: int K2SE_SpawnType(int nIndex)   OBJECT_TYPE_PLACEABLE (64) or OBJECT_TYPE_CREATURE (1)
int H_SpawnType(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    return PopIndexPushInt(&spawner::Type);
#else
    return 0;
#endif
}
// 902: string K2SE_SpawnTemplate(int nIndex)
int H_SpawnTemplate(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int index = 0;
    if (!PopInt(&index)) return off::kErrParam;
    ExoString value(spawner::Template(index));
    if (!value.valid()) return off::kErrParam;
    if (!PushString(&value)) return off::kErrPushFailed;
#endif
    return 0;
}
// 903..906: float K2SE_SpawnX/Y/Z/Facing(int nIndex)
int H_SpawnX(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    return PopIndexPushFloat(&spawner::X);
#else
    return 0;
#endif
}
int H_SpawnY(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    return PopIndexPushFloat(&spawner::Y);
#else
    return 0;
#endif
}
int H_SpawnZ(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    return PopIndexPushFloat(&spawner::Z);
#else
    return 0;
#endif
}
int H_SpawnFacing(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    return PopIndexPushFloat(&spawner::Facing);
#else
    return 0;
#endif
}
// 907: int K2SE_SpawnReport(int nIndex, int bCreated)
int H_SpawnReport(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int index = 0, ok = 0;
    if (!PopInt(&index)) return off::kErrParam;
    if (!PopInt(&ok)) return off::kErrParam;
    if (!PushInt(spawner::Report(index, ok != 0) ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}
// 908: int K2SE_GetVarietyStatus()
int H_GetVarietyStatus(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(npcvariety::Status())) return off::kErrPushFailed;
#endif
    return 0;
}

// --- living-kotor2 bridge: 909..910 ---------------------------------------------
// The string ABI these ride on is the one EchoString proved on 2026-10-09; the
// socket and the queues live in bridge.cpp and never touch the VM stack.

// 909: int LK2_Post(string sText)
int H_LK2Post(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    ExoString value;
    if (!value.valid()) return off::kErrParam;
    if (!PopString(&value)) return off::kErrParam;
    const int id = bridge::Post(value.c_str(), value.length());
    if (!PushInt(id)) return off::kErrPushFailed;
#endif
    return 0;
}

// 910: string LK2_Poll(int nId)
int H_LK2Poll(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int nId = 0;
    if (!PopInt(&nId)) return off::kErrParam;
    static char buf[lk2::kMaxText];  // game thread only
    uint32_t len = 0;
    if (!bridge::Poll(nId, buf, sizeof(buf), &len)) buf[0] = '\0';
    ExoString out(buf);
    if (!out.valid()) return off::kErrParam;
    if (!PushString(&out)) return off::kErrPushFailed;
#endif
    return 0;
}

struct Extended {
    int id;
    const char* name;
    int argc;  // Q7: the compiler always emits the full declared argc,
               // materializing omitted defaults itself -- so a mismatch here
               // means a foreign or corrupted script, and we refuse to touch
               // the VM stack rather than desynchronize it.
    ExtendedHandler handler;
};

constexpr Extended kExtended[] = {
    {877, "K2SE_GetVersion", 0, &H_GetVersion},
    {878, "K2SE_SelfTest", 3, &H_SelfTest},
    {879, "K2SE_ReportTest", 3, &H_ReportTest},
    {880, "K2SE_EchoString", 1, &H_EchoString},
    {881, "K2SE_GetAbilityScoreBase", 2, &H_GetAbilityScoreBase},
    {882, "K2SE_GetSkillRankBase", 2, &H_GetSkillRankBase},
    {883, "K2SE_GetFeatAcquired", 2, &H_GetFeatAcquired},
    {884, "K2SE_GetSpellAcquired", 2, &H_GetSpellAcquired},
    {885, "K2SE_SetFogEnabled", 1, &H_SetFogEnabled},
    {886, "K2SE_SetFogRange", 2, &H_SetFogRange},
    {887, "K2SE_SetFogColor", 3, &H_SetFogColor},
    {888, "K2SE_GetFogStatus", 0, &H_GetFogStatus},
    {889, "K2SE_GetMovementStatus", 0, &H_GetMovementStatus},
    {890, "K2SE_SetSprintFactor", 1, &H_SetSprintFactor},
    {891, "K2SE_SetCrouch", 1, &H_SetCrouch},
    {892, "K2SE_Jump", 0, &H_Jump},
    {893, "K2SE_Roll", 0, &H_Roll},
    {894, "K2SE_SetFOV", 2, &H_SetFOV},
    {895, "K2SE_GetFOV", 0, &H_GetFOV},
    {896, "K2SE_SetCameraView", 1, &H_SetCameraView},
    {897, "K2SE_GetCameraView", 0, &H_GetCameraView},
    {898, "K2SE_SpawnBegin", 2, &H_SpawnBegin},
    {899, "K2SE_SpawnMarkPresent", 1, &H_SpawnMarkPresent},
    {900, "K2SE_SpawnNeeded", 1, &H_SpawnNeeded},
    {901, "K2SE_SpawnType", 1, &H_SpawnType},
    {902, "K2SE_SpawnTemplate", 1, &H_SpawnTemplate},
    {903, "K2SE_SpawnX", 1, &H_SpawnX},
    {904, "K2SE_SpawnY", 1, &H_SpawnY},
    {905, "K2SE_SpawnZ", 1, &H_SpawnZ},
    {906, "K2SE_SpawnFacing", 1, &H_SpawnFacing},
    {907, "K2SE_SpawnReport", 2, &H_SpawnReport},
    {908, "K2SE_GetVarietyStatus", 0, &H_GetVarietyStatus},
    // living-kotor2 bridge -- ids allocated from 909 upward
    {909, "LK2_Post", 1, &H_LK2Post},
    {910, "LK2_Poll", 1, &H_LK2Poll},
};
constexpr int kExtendedCount = static_cast<int>(sizeof(kExtended) / sizeof(kExtended[0]));

// g_dispatchCount is indexed by position in this table. Growing the table past
// the counter array must be a compile error, not a stray write past the end.
static_assert(kExtendedCount <= static_cast<int>(sizeof(g_dispatchCount) /
                                                sizeof(g_dispatchCount[0])),
              "g_dispatchCount is too small for kExtended -- grow it");

}  // namespace

void Init() {
    log::Writef("extended routines registered: %d (first free vanilla ID is %d)", kExtendedCount,
                off::kFirstExtendedId);
    for (const Extended& e : kExtended)
        log::Writef("  id %d -> %s (argc %d)", e.id, e.name, e.argc);
    log::Writef("stack ABI: %s",
                K2SE_ENABLE_STACK_ABI ? "ENABLED" : "disabled (DESIGN.md Q5 unresolved)");
}

bool Intercepts(int id) { return id == off::kRoutineAbs; }

bool DispatchVanillaOverride(void* self, int id, int nParams, int* result) {
    if (id != off::kRoutineAbs) return false;

#if !K2SE_ENABLE_STACK_ABI
    // Without a verified stack ABI we must not touch arguments. Fall through to
    // the engine so abs() keeps working exactly as it always did.
    (void)self;
    (void)nParams;
    (void)result;
    if (!g_probeSeen) {
        g_probeSeen = true;
        log::Write("abs() intercept reached, but the stack ABI is disabled -> passing through.");
    }
    return false;
#else
    (void)self;
    // abs() takes exactly one argument and has no defaults, so any other argc
    // is not the routine we think it is -- hand it back to the engine.
    if (nParams != 1) return false;

    int value = 0;
    if (!PopInt(&value)) {
        log::Write("abs() intercept: StackPopInteger failed");
        *result = off::kErrParam;  // same code the engine's own handler returns
        return true;
    }

    if (value == kProbeMagic) {
        if (!g_probeSeen) {
            g_probeSeen = true;
            log::Writef("PRESENCE PROBE answered with version %d", kVersionEncoded);
        }
        PostBanner(kExtendedCount);
        if (!PushInt(kVersionEncoded)) {
            *result = off::kErrPushFailed;
            return true;
        }
        *result = 0;
        return true;
    }

    // Faithful reimplementation for every other input. abs() is pure, has no
    // default parameters and argc is always 1, so this is bit-identical to
    // vanilla (abs(INT_MIN) is UB in both).
    if (!PushInt(value < 0 ? -value : value)) {
        *result = off::kErrPushFailed;
        return true;
    }
    *result = 0;
    return true;
#endif
}

int DispatchExtended(void* /*self*/, int id, int nParams) {
    for (const Extended& e : kExtended) {
        if (e.id != id) continue;
        if (!g_extendedSeen) {
            g_extendedSeen = true;
            log::Writef("*** FIRST EXTENDED ROUTINE CALL: id=%d (%s), nParams=%d ***", id, e.name,
                        nParams);
            log::Write("*** a compiled NWScript reached a routine that did not exist "
                       "in this engine. ***");
        }
        if (nParams != e.argc) {
            // Q7 guarantees the compiler emits the full declared argc, so this
            // is a script compiled against a mismatched header. Popping an
            // unknown number of values would desynchronize the VM stack;
            // refuse instead. -2001 aborts only the calling script.
            log::Writef("%s called with nParams=%d, declared argc=%d -> refusing (-2001)", e.name,
                        nParams, e.argc);
            return off::kErrParam;
        }
        // Thinned, because unthinned this was the whole log. A traced session on
        // 2026-08-30 wrote 205,808 of these two lines and reached 9 MB, which
        // buries the handful of lines anyone actually opens the file for. The
        // first few calls of each routine are where the interesting information
        // is -- that it dispatched at all, with what argc, returning what -- and
        // after that it is the same line forever. So: the first eight in full,
        // then one in every 1024 carrying the running count.
        const size_t slot = static_cast<size_t>(&e - kExtended);
        const uint32_t n = ++g_dispatchCount[slot];
        const bool loud = (n <= 8) || ((n & 1023) == 0);

        if (loud) log::Trace("extended routine %d (%s), nParams=%d [call %u]", id, e.name,
                             nParams, n);
        const int rc = e.handler(nParams);
        if (loud) log::Trace("  -> rc=%d", rc);
        PostBanner(kExtendedCount);
        return rc;
    }

    // Unregistered extended ID: return exactly what the engine returns for an
    // out-of-range routine, so behaviour with K2SE matches behaviour without it.
    log::Trace("unregistered extended routine %d -> %d", id, off::kErrCommandNotFound);
    return off::kErrCommandNotFound;
}

}  // namespace routines
}  // namespace k2se
