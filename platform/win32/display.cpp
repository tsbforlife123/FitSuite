#include "win32_platform.hpp"

#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace fitapp::win32 {

namespace {

struct TargetName {
    std::string friendly;
    std::string edid;
    std::string connector;
    double refresh_hz = 0.0;
};

std::string connector_name(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t) {
    switch (t) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return "DisplayPort";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return "DisplayPort (internal)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return "DVI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "Internal";
        default: return "";
    }
}

// Maps \\.\DISPLAYn -> monitor name, EDID ids and the exact refresh rate of the active path.
std::map<std::string, TargetName> query_targets() {
    std::map<std::string, TargetName> out;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return out;
    for (UINT32 i = 0; i < np; ++i) {
        const auto& p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof tgt;
        tgt.header.adapterId = p.targetInfo.adapterId;
        tgt.header.id = p.targetInfo.id;
        TargetName t;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) {
            t.friendly = narrow(tgt.monitorFriendlyDeviceName);
            if (tgt.flags.edidIdsValid) {
                // EDID manufacturer id: three 5-bit letters, big-endian in the 16-bit value.
                const UINT16 m = UINT16((tgt.edidManufactureId >> 8) | (tgt.edidManufactureId << 8));
                char id[16];
                std::snprintf(id, sizeof id, "%c%c%c%04X", '@' + ((m >> 10) & 31), '@' + ((m >> 5) & 31), '@' + (m & 31),
                              unsigned(tgt.edidProductCodeId));
                t.edid = id;
            }
            t.connector = connector_name(tgt.outputTechnology);
        }
        const auto& rr = p.targetInfo.refreshRate;
        if (rr.Denominator) t.refresh_hz = double(rr.Numerator) / double(rr.Denominator);
        out[narrow(src.viewGdiDeviceName)] = t;
    }
    return out;
}

std::string probe_ddc(HMONITOR mon) {
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || n == 0) return "unsupported";
    std::vector<PHYSICAL_MONITOR> pm(n);
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm.data())) return "unsupported";
    DWORD len = 0;
    const bool ok = GetCapabilitiesStringLength(pm[0].hPhysicalMonitor, &len) && len > 0;
    DestroyPhysicalMonitors(n, pm.data());
    return ok ? "supported" : "unsupported";
}

struct MonitorLookup {
    std::wstring device;
    HMONITOR found = nullptr;
};

BOOL CALLBACK find_monitor(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    auto* l = reinterpret_cast<MonitorLookup*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(m, &mi) && l->device == mi.szDevice) {
        l->found = m;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

std::string probe_ddcci_device(const std::string& device) {
    MonitorLookup l{widen(device)};
    EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&l));
    return l.found ? probe_ddc(l.found) : "unknown";
}

std::vector<DisplayInfo> enumerate_displays(HWND window, const std::map<std::string, std::string>& ddc_cache) {
    std::vector<DisplayInfo> out;
    const auto targets = query_targets();
    std::wstring window_device;
    {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi)) window_device = mi.szDevice;
    }
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DisplayInfo d;
        d.device = narrow(dd.DeviceName);
        d.primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        d.hosts_window = window_device == dd.DeviceName;
        DEVMODEW cur{};
        cur.dmSize = sizeof cur;
        if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &cur, 0)) {
            d.width = int(cur.dmPelsWidth);
            d.height = int(cur.dmPelsHeight);
            d.refresh_hz = cur.dmDisplayFrequency;
        }
        std::set<int> hz;
        for (DWORD m = 0;; ++m) {
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (!EnumDisplaySettingsExW(dd.DeviceName, m, &dm, 0)) break;
            if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight && dm.dmBitsPerPel == cur.dmBitsPerPel &&
                dm.dmDisplayFrequency > 1)
                hz.insert(int(dm.dmDisplayFrequency));
        }
        for (int h : hz) d.modes_hz.push_back(double(h));
        auto it = targets.find(d.device);
        if (it != targets.end()) {
            d.name = it->second.friendly;
            d.edid_id = it->second.edid;
            d.connector = it->second.connector;
            if (it->second.refresh_hz > 0) d.refresh_hz = it->second.refresh_hz;
        }
        if (d.name.empty()) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof mon;
            if (EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0)) d.name = narrow(mon.DeviceString);
        }
        auto cached = ddc_cache.find(d.device);
        d.ddcci = cached != ddc_cache.end() ? cached->second : "unknown";
        out.push_back(d);
    }
    return out;
}

bool change_refresh(const std::string& device, double hz, bool persistent, std::string* error) {
    const std::wstring dev = widen(device);
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsExW(dev.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) {
        if (error) *error = "the display's current mode could not be read";
        return false;
    }
    // Windows lists integer rates (e.g. 239 or 240 for a 239.76 Hz panel); pick the closest one.
    int best = int(std::lround(hz));
    double best_d = 1e9;
    for (DWORD m = 0;; ++m) {
        DEVMODEW x{};
        x.dmSize = sizeof x;
        if (!EnumDisplaySettingsExW(dev.c_str(), m, &x, 0)) break;
        if (x.dmPelsWidth != dm.dmPelsWidth || x.dmPelsHeight != dm.dmPelsHeight) continue;
        const double d = std::fabs(double(x.dmDisplayFrequency) - hz);
        if (d < best_d) {
            best_d = d;
            best = int(x.dmDisplayFrequency);
        }
    }
    dm.dmDisplayFrequency = DWORD(best);
    dm.dmFields = DM_DISPLAYFREQUENCY | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (ChangeDisplaySettingsExW(dev.c_str(), &dm, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        if (error) *error = "Windows reports that this mode is not supported";
        return false;
    }
    // CDS_FULLSCREEN = temporary: Windows restores the saved mode when FitSuite exits.
    const DWORD flags = persistent ? CDS_UPDATEREGISTRY : CDS_FULLSCREEN;
    const LONG r = ChangeDisplaySettingsExW(dev.c_str(), &dm, nullptr, flags, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        if (error) *error = "ChangeDisplaySettingsEx failed (" + std::to_string(r) + ")";
        return false;
    }
    return true;
}

double window_refresh_hz(HWND window) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi)) return 0.0;
    const auto targets = query_targets();
    auto it = targets.find(narrow(mi.szDevice));
    if (it != targets.end() && it->second.refresh_hz > 1.0) return it->second.refresh_hz;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0) && dm.dmDisplayFrequency > 1)
        return double(dm.dmDisplayFrequency);
    return 0.0;
}

bool restore_registry_mode(const std::string& device, std::string* error) {
    const LONG r = ChangeDisplaySettingsExW(widen(device).c_str(), nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        if (error) *error = "restoring the saved display mode failed (" + std::to_string(r) + ")";
        return false;
    }
    return true;
}

}  // namespace fitapp::win32
