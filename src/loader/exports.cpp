#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

extern "C" {

extern HMODULE g_real_dinput8;

HRESULT WINAPI ForwardDirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
    LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    using Func_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DirectInput8Create"));
    return real(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

HRESULT WINAPI ForwardDirectInputCreateA(
    HINSTANCE hinst, DWORD dwVersion, LPVOID* ppDI, LPUNKNOWN punkOuter)
{
    using Func_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, LPVOID*, LPUNKNOWN);
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DirectInputCreateA"));
    return real(hinst, dwVersion, ppDI, punkOuter);
}

HRESULT WINAPI ForwardDirectInputCreateW(
    HINSTANCE hinst, DWORD dwVersion, LPVOID* ppDI, LPUNKNOWN punkOuter)
{
    using Func_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, LPVOID*, LPUNKNOWN);
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DirectInputCreateW"));
    return real(hinst, dwVersion, ppDI, punkOuter);
}

HRESULT WINAPI ForwardDirectInputCreateEx(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
    LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    using Func_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DirectInputCreateEx"));
    return real(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

HRESULT WINAPI ForwardDllCanUnloadNow()
{
    using Func_t = HRESULT(WINAPI*)();
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DllCanUnloadNow"));
    return real();
}

HRESULT WINAPI ForwardDllGetClassObject(
    REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    using Func_t = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DllGetClassObject"));
    return real(rclsid, riid, ppv);
}

HRESULT WINAPI ForwardDllRegisterServer()
{
    using Func_t = HRESULT(WINAPI*)();
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DllRegisterServer"));
    return real();
}

HRESULT WINAPI ForwardDllUnregisterServer()
{
    using Func_t = HRESULT(WINAPI*)();
    static Func_t real = nullptr;
    if (!real)
        real = reinterpret_cast<Func_t>(
            GetProcAddress(g_real_dinput8, "DllUnregisterServer"));
    return real();
}

}
