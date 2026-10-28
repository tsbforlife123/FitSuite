#include "win32_platform.hpp"

#include <dxgi1_6.h>

#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace fitapp::win32 {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

int64_t qpc_now_us() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return int64_t((c.QuadPart / freq) * 1000000 + (c.QuadPart % freq) * 1000000 / freq);
}

namespace {

std::string cpu_brand() {
    unsigned regs[12] = {};
#if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 0x80000000);
    if (unsigned(r[0]) < 0x80000004) return "";
    for (int i = 0; i < 3; ++i) {
        __cpuid(r, 0x80000002 + i);
        std::memcpy(&regs[i * 4], r, 16);
    }
#else
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000000, &a, &b, &c, &d) || a < 0x80000004) return "";
    for (unsigned i = 0; i < 3; ++i) {
        __get_cpuid(0x80000002 + i, &a, &b, &c, &d);
        regs[i * 4 + 0] = a;
        regs[i * 4 + 1] = b;
        regs[i * 4 + 2] = c;
        regs[i * 4 + 3] = d;
    }
#endif
    char s[49] = {};
    std::memcpy(s, regs, 48);
    std::string out(s);
    const size_t a0 = out.find_first_not_of(' ');
    return a0 == std::string::npos ? "" : out.substr(a0);
}

std::wstring reg_string(HKEY root, const wchar_t* path, const wchar_t* value) {
    wchar_t buf[256];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, path, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

DWORD reg_dword(HKEY root, const wchar_t* path, const wchar_t* value) {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(root, path, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return 0;
    return v;
}

}  // namespace

std::string os_display_name() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) fn(&v);
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::wstring disp = reg_string(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
    const DWORD ubr = reg_dword(HKEY_LOCAL_MACHINE, key, L"UBR");
    std::string name = v.dwMajorVersion >= 10 ? (v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") : "Windows";
    if (!disp.empty()) name += " " + narrow(disp);
    name += " (build " + std::to_string(v.dwBuildNumber) + (ubr ? "." + std::to_string(ubr) : "") + ")";
    return name;
}

fit::PcInfo query_pc_info() {
    fit::PcInfo pc;
    pc.cpu = cpu_brand();
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) pc.ram_gb = double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    pc.os = os_display_name();
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                fit::GpuInfo g;
                g.name = narrow(d.Description);
                g.vendor_id = d.VendorId;
                g.device_id = d.DeviceId;
                g.vram_gb = double(d.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0);
                LARGE_INTEGER umd{};
                if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                    const auto q = uint64_t(umd.QuadPart);
                    g.driver = std::to_string((q >> 48) & 0xFFFF) + "." + std::to_string((q >> 32) & 0xFFFF) + "." +
                               std::to_string((q >> 16) & 0xFFFF) + "." + std::to_string(q & 0xFFFF);
                }
                pc.gpus.push_back(g);
            }
            a->Release();
        }
        f->Release();
    }
    pc.source = "detected";
    return pc;
}

}  // namespace fitapp::win32
