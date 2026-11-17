/**
 * test_bondriver.cpp: BonDriver_WebOS.dll を実機 TV に対してエンドツーエンド検証するテストツール
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "IBonDriver2.h"
#include <cstdio>
#include <chrono>

typedef IBonDriver *(*PFN_CreateBonDriver)(void);

static bool captureTs(IBonDriver2 *drv, const char *outPath, int millis) {
    FILE *fp = std::fopen(outPath, "wb");
    if (!fp) return false;
    auto t0 = std::chrono::steady_clock::now();
    size_t total = 0;
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= millis) break;

        if (drv->WaitTsStream(500) == WAIT_OBJECT_0) {
            BYTE *ptr = nullptr;
            DWORD sz = 0, rem = 0;
            while (drv->GetTsStream(&ptr, &sz, &rem)) {
                if (ptr && sz > 0) {
                    std::fwrite(ptr, 1, sz, fp);
                    total += sz;
                }
                if (rem == 0) break;
            }
        }
    }
    std::fclose(fp);
    std::printf("Captured %s: %zu bytes (sig=%.1f)\n", outPath, total, drv->GetSignalLevel());
    return total > 188000;
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *dllPath = (argc >= 2) ? argv[1] : L"BonDriver_WebOS.dll";
    HMODULE mod = LoadLibraryW(dllPath);
    if (!mod) {
        std::printf("LoadLibrary failed: err=%lu\n", GetLastError());
        return 1;
    }
    auto createFn = reinterpret_cast<PFN_CreateBonDriver>(GetProcAddress(mod, "CreateBonDriver"));
    if (!createFn) {
        std::printf("GetProcAddress(CreateBonDriver) failed\n");
        FreeLibrary(mod);
        return 1;
    }

    IBonDriver *base = createFn();
    IBonDriver2 *drv = static_cast<IBonDriver2 *>(base);
    if (!drv || !drv->OpenTuner()) {
        std::printf("OpenTuner failed\n");
        if (drv) drv->Release();
        FreeLibrary(mod);
        return 1;
    }

    std::wprintf(L"Tuner: %s (opening=%d)\n", drv->GetTunerName(), drv->IsTunerOpening());
    for (DWORD s = 0; ; s++) {
        LPCWSTR sp = drv->EnumTuningSpace(s);
        if (!sp) break;
        DWORD cnt = 0;
        while (drv->EnumChannelName(s, cnt)) cnt++;
        std::wprintf(L"  Space %lu [%s]: %lu channels\n", s, sp, cnt);
        if (cnt > 0) {
            std::wprintf(L"    [0] %s\n", drv->EnumChannelName(s, 0));
        }
    }

    if (!drv->SetChannel(0, 0)) {
        std::printf("SetChannel(0,0) failed\n");
        drv->CloseTuner();
        drv->Release();
        FreeLibrary(mod);
        return 1;
    }
    bool ok0 = captureTs(drv, "bondriver_gr.ts", 2000);

    bool ok1 = true;
    if (drv->EnumChannelName(1, 0)) {
        if (!drv->SetChannel(1, 0)) {
            std::printf("SetChannel(1,0) failed\n");
            ok1 = false;
        } else {
            ok1 = captureTs(drv, "bondriver_bs.ts", 2000);
        }
    }

    drv->CloseTuner();
    drv->Release();
    FreeLibrary(mod);
    std::printf("Result: ok0=%d ok1=%d\n", ok0 ? 1 : 0, ok1 ? 1 : 0);
    return (ok0 && ok1) ? 0 : 1;
}
