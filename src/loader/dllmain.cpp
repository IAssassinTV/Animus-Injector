#include <windows.h>
#include <vector>

extern "C" HMODULE g_real_dinput8 = nullptr;

namespace {

std::vector<HMODULE> g_asi_modules;

void load_real_dinput8() {
    wchar_t sys_path[MAX_PATH];
    if (!GetSystemDirectoryW(sys_path, MAX_PATH)) return;
    wcscat_s(sys_path, L"\\dinput8.dll");
    g_real_dinput8 = LoadLibraryW(sys_path);
}

void load_asi_files() {
    wchar_t exe_path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) return;

    wchar_t* last_slash = wcsrchr(exe_path, L'\\');
    if (!last_slash) return;
    *last_slash = L'\0';

    wchar_t search_path[MAX_PATH];
    wcscpy_s(search_path, exe_path);
    wcscat_s(search_path, L"\\*.asi");

    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(search_path, &find_data);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        wchar_t asi_path[MAX_PATH];
        wcscpy_s(asi_path, exe_path);
        wcscat_s(asi_path, L"\\");
        wcscat_s(asi_path, find_data.cFileName);

        HMODULE mod = LoadLibraryW(asi_path);
        if (mod) g_asi_modules.push_back(mod);
    } while (FindNextFileW(find, &find_data));

    FindClose(find);
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        load_real_dinput8();
        load_asi_files();
    } else if (reason == DLL_PROCESS_DETACH) {
        for (auto mod : g_asi_modules)
            FreeLibrary(mod);
        g_asi_modules.clear();
        if (g_real_dinput8) {
            FreeLibrary(g_real_dinput8);
            g_real_dinput8 = nullptr;
        }
    }
    return TRUE;
}
