#include <windows.h>

#include "config.h"
#include "fingerprint.h"
#include "glhook.h"
#include "log.h"
#include "movement.h"
#include "fov.h"
#include "camera.h"
#include "fpcam.h"
#include "fpview.h"
#include "console.h"
#include "render.h"
#include "spawner.h"
#include "npcvariety.h"
#include "offsets.h"
#include "routines.h"
#include "vm.h"
#include "bridge.h"

// The proxy stubs resolve the real version.dll lazily, on first call -- not
// here. LoadLibrary from DllMain runs under the loader lock and is exactly the
// pattern MSDN warns about.
//
// The VM hook, by contrast, IS installed here: it needs only VirtualProtect and
// a dword store (both loader-lock safe), and DllMain runs before the EXE entry
// point at RVA 0x0051D5A2, therefore before CSWVirtualMachineCommands is ever
// constructed. That ordering is what makes a vtable-slot swap sufficient.

namespace {

void OnAttach(HMODULE self) {
    DisableThreadLibraryCalls(self);

    k2se::log::Init();
    k2se::fingerprint::LogEnvironment();

    const auto verdict = k2se::fingerprint::Check();
    if (verdict != k2se::fingerprint::Verdict::kMatch) {
        k2se::log::Writef("K2SE_LOAD_REFUSED: %s", k2se::fingerprint::LastFailure());
        k2se::log::Write("Nothing was hooked. The game runs vanilla.");
        return;  // fail safe: an inert hook is worse than no hook
    }

    k2se::log::Write("fingerprint OK");
    k2se::routines::Init();

    // Parsed here rather than further down because glhook and render both
    // decide whether to patch anything from the ini, and they run next.
    k2se::config::Load();
    k2se::config::LogAll();
    k2se::render::Install();

    // Opt-in and self-contained: if fog support refuses to install, the VM hook
    // is unaffected and K2SE still loads. It is deliberately attempted after the
    // fingerprint has passed, so it never touches an unrecognised binary.
    k2se::glhook::Install();

    if (k2se::vm::InstallHook()) {
        k2se::log::Write("K2SE_LOAD_OK");
    } else {
        k2se::log::Writef("K2SE_LOAD_REFUSED: hook installation failed");
        return;
    }

    // Movement features (K2 Jump / Crouch / Sprint). Entirely driven by
    // k2se_movement.ini next to the exe: no file, no redirected call sites.
    // Installed last, after the VM hook is in, so a refusal here leaves a fully
    // working K2SE 0.1 behind it.
    k2se::movement::Install();
    k2se::fov::Install();
    k2se::camera::Install();
    k2se::fpcam::Install();
    k2se::fpview::Install();
    k2se::spawner::Install();
    k2se::npcvariety::Install();
    k2se::console::Install();
}

void OnDetach() {
    k2se::bridge::Remove();
    k2se::console::Remove();
    k2se::npcvariety::Remove();
    k2se::spawner::Remove();
    k2se::fpview::Remove();
    k2se::fpcam::Remove();
    k2se::camera::Remove();
    k2se::fov::Remove();
    k2se::movement::Remove();
    k2se::glhook::Remove();
    k2se::render::Remove();
    k2se::vm::RemoveHook();
    k2se::log::Shutdown();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            OnAttach(module);
            break;
        case DLL_PROCESS_DETACH:
            OnDetach();
            break;
        default:
            break;
    }
    return TRUE;
}
