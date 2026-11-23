// DeviceInventory.cpp
// Native Win32 console inventory tool. Intended for an x86 build with an XP-compatible toolset.
// Uses WMI (available on Windows XP and later) and does not require PowerShell or .NET.

#define _WIN32_DCOM
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#include <oleauto.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <cstdlib>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

struct WmiRow {
    std::wstring model;
    std::wstring type;
    unsigned long long capacityGB;
};

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring VariantString(const VARIANT& v) {
    if (v.vt == VT_BSTR && v.bstrVal) return v.bstrVal;
    if (v.vt == VT_I2) { std::wostringstream o; o << v.iVal; return o.str(); }
    if (v.vt == VT_I4) { std::wostringstream o; o << v.lVal; return o.str(); }
    if (v.vt == VT_UI4) { std::wostringstream o; o << v.ulVal; return o.str(); }
    if (v.vt == VT_UI8) { std::wostringstream o; o << v.ullVal; return o.str(); }
    if (v.vt == VT_I8) { std::wostringstream o; o << v.llVal; return o.str(); }
    if (v.vt == VT_BOOL) return v.boolVal == VARIANT_TRUE ? L"True" : L"False";
    return L"";
}

static unsigned long long VariantNumber(const VARIANT& v) {
    if (v.vt == VT_UI8) return v.ullVal;
    if (v.vt == VT_I8) return (unsigned long long)v.llVal;
    if (v.vt == VT_UI4) return v.ulVal;
    if (v.vt == VT_I4) return (unsigned long long)v.lVal;
    if (v.vt == VT_I2) return (unsigned long long)v.iVal;
    if (v.vt == VT_BSTR && v.bstrVal) {
        wchar_t* end = 0;
        return _wcstoui64(v.bstrVal, &end, 10);
    }
    return 0;
}

class Wmi {
public:
    IWbemServices* services;
    Wmi() : services(0) {}
    ~Wmi() { if (services) services->Release(); }

    bool Connect() {
        IWbemLocator* locator = 0;
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                                      IID_IWbemLocator, (void**)&locator);
        if (FAILED(hr)) return false;

        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        hr = locator->ConnectServer(ns, 0, 0, 0, 0, 0, 0, &services);
        SysFreeString(ns);
        locator->Release();
        if (FAILED(hr)) return false;

        hr = CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, 0,
                               RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                               0, EOAC_NONE);
        return SUCCEEDED(hr);
    }

    std::vector<IWbemClassObject*> Query(const wchar_t* query) {
        std::vector<IWbemClassObject*> rows;
        if (!services) return rows;
        IEnumWbemClassObject* enumerator = 0;
        BSTR language = SysAllocString(L"WQL");
        BSTR statement = SysAllocString(query);
        HRESULT hr = services->ExecQuery(language, statement,
                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                    0, &enumerator);
        SysFreeString(language);
        SysFreeString(statement);
        if (FAILED(hr) || !enumerator) return rows;

        while (true) {
            IWbemClassObject* obj = 0;
            ULONG returned = 0;
            hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned);
            if (FAILED(hr) || returned == 0) break;
            rows.push_back(obj);
        }
        enumerator->Release();
        return rows;
    }
};

static std::wstring GetProp(IWbemClassObject* obj, const wchar_t* name) {
    VARIANT v; VariantInit(&v);
    std::wstring result;
    if (SUCCEEDED(obj->Get(name, 0, &v, 0, 0))) result = VariantString(v);
    VariantClear(&v);
    return result;
}

static unsigned long long GetNum(IWbemClassObject* obj, const wchar_t* name) {
    VARIANT v; VariantInit(&v);
    unsigned long long result = 0;
    if (SUCCEEDED(obj->Get(name, 0, &v, 0, 0))) result = VariantNumber(v);
    VariantClear(&v);
    return result;
}

static std::wstring RamTypeName(unsigned long long n) {
    switch ((int)n) {
        case 20: return L"DDR";
        case 21: return L"DDR2";
        case 22: return L"DDR2 FB-DIMM";
        case 24: return L"DDR3";
        case 26: return L"DDR4";
        case 34: return L"DDR5";
        default: return L"Unknown";
    }
}

static bool ContainsNoCase(std::wstring s, const std::wstring& part) {
    std::transform(s.begin(), s.end(), s.begin(), towlower);
    std::wstring p = part;
    std::transform(p.begin(), p.end(), p.begin(), towlower);
    return s.find(p) != std::wstring::npos;
}

static std::wstring DetectDiskType(const std::wstring& model) {
    if (ContainsNoCase(model, L"SSD") ||
        ContainsNoCase(model, L"Solid State") ||
        ContainsNoCase(model, L"NVMe")) return L"SSD";
    // WMI on older Windows does not expose a reliable universal SSD/HDD property.
    return L"Unknown";
}

static std::string Utf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
                                0, 0, 0, 0);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
                         &out[0], n, 0, 0);
    return out;
}

static std::wstring CsvQuote(const std::wstring& s) {
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'"') out += L"\"\"";
        else out += s[i];
    }
    out += L"\"";
    return out;
}

static bool WriteCsv(const std::wstring& path, const std::vector<std::wstring>& fields) {
    DWORD attr = GetFileAttributesW(path.c_str());
    bool exists = (attr != INVALID_FILE_ATTRIBUTES);
    std::ofstream f(Utf8(path).c_str(), std::ios::binary | std::ios::app);
    if (!f) return false;
    if (!exists) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        f.write((const char*)bom, 3);
        f << "ComputerName,Manufacturer,Model,CPU,RAMGB,RAMType,RAMSlots,RAMSlotsFree,HardDisk\r\n";
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) f << ",";
        f << "\"" ;
        std::string val = Utf8(fields[i]);
        for (size_t j = 0; j < val.size(); ++j) {
            if (val[j] == '"') f << "\"\"";
            else f << val[j];
        }
        f << "\"";
    }
    f << "\r\n";
    return f.good();
}

int wmain() {
    const unsigned long long TargetRAMGB = 16;
    const unsigned long long TargetSSDGB = 256;

    SetConsoleTitleW(L"Device Inventory & Upgrade Recommendation");
    SetConsoleOutputCP(CP_UTF8);

    HRESULT init = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(init)) {
        std::wcout << L"Unable to initialize Windows Management Instrumentation.\n";
        std::wcout << L"Press any key to exit...";
        std::wcin.get();
        return 1;
    }

    Wmi wmi;
    if (!wmi.Connect()) {
        std::wcout << L"Unable to connect to WMI. Check that the Windows Management Instrumentation service is running.\n";
        std::wcout << L"Press any key to exit...";
        std::wcin.get();
        CoUninitialize();
        return 1;
    }

    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD computerNameSize = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(computerName, &computerNameSize);

    std::wstring manufacturer, model, cpuName;
    std::vector<IWbemClassObject*> rows = wmi.Query(L"SELECT Manufacturer, Model FROM Win32_ComputerSystem");
    if (!rows.empty()) {
        manufacturer = GetProp(rows[0], L"Manufacturer");
        model = GetProp(rows[0], L"Model");
    }
    for (size_t i = 0; i < rows.size(); ++i) rows[i]->Release();

    rows = wmi.Query(L"SELECT Name FROM Win32_Processor");
    if (!rows.empty()) cpuName = GetProp(rows[0], L"Name");
    for (size_t i = 0; i < rows.size(); ++i) rows[i]->Release();

    unsigned long long totalBytes = 0;
    std::vector<unsigned long long> ramTypes;
    unsigned long long ramModules = 0;
    rows = wmi.Query(L"SELECT Capacity, MemoryType FROM Win32_PhysicalMemory");
    for (size_t i = 0; i < rows.size(); ++i) {
        totalBytes += GetNum(rows[i], L"Capacity");
        unsigned long long t = GetNum(rows[i], L"MemoryType");
        if (std::find(ramTypes.begin(), ramTypes.end(), t) == ramTypes.end()) ramTypes.push_back(t);
        ++ramModules;
        rows[i]->Release();
    }
    unsigned long long totalRAMGB = (totalBytes + (1024ULL*1024*1024/2)) / (1024ULL*1024*1024);

    unsigned long long slotsTotal = 0;
    rows = wmi.Query(L"SELECT MemoryDevices FROM Win32_PhysicalMemoryArray");
    for (size_t i = 0; i < rows.size(); ++i) {
        slotsTotal += GetNum(rows[i], L"MemoryDevices");
        rows[i]->Release();
    }
    if (slotsTotal == 0) slotsTotal = ramModules;
    unsigned long long slotsFree = slotsTotal > ramModules ? slotsTotal - ramModules : 0;

    std::wstring ramType = L"Unknown";
    for (size_t i = 0; i < ramTypes.size(); ++i) ramType = RamTypeName(ramTypes[i]);

    unsigned long long ramNeeded = totalRAMGB < TargetRAMGB ? TargetRAMGB - totalRAMGB : 0;
    std::wstring ramAction;
    if (totalRAMGB >= TargetRAMGB) ramAction = L"OK";
    else if (slotsFree > 0) {
        std::wostringstream s; s << L"ADD " << ramNeeded << L" GB RAM"; ramAction = s.str();
    } else ramAction = L"REPLACE RAM WITH 16 GB";

    std::wostringstream ramDisplayStream;
    ramDisplayStream << totalRAMGB << L" GB " << ramType;
    std::wstring ramDisplay = ramDisplayStream.str();

    std::vector<WmiRow> disks;
    rows = wmi.Query(L"SELECT Model, Size FROM Win32_DiskDrive");
    for (size_t i = 0; i < rows.size(); ++i) {
        WmiRow d;
        d.model = GetProp(rows[i], L"Model");
        unsigned long long bytes = GetNum(rows[i], L"Size");
        d.capacityGB = (bytes + (1024ULL*1024*1024/2)) / (1024ULL*1024*1024);
        d.type = DetectDiskType(d.model);
        disks.push_back(d);
        rows[i]->Release();
    }

    std::wstring storageDisplay;
    bool hasSuitableSSD = false;
    for (size_t i = 0; i < disks.size(); ++i) {
        if (!storageDisplay.empty()) storageDisplay += L" | ";
        std::wostringstream s; s << disks[i].type << L" " << disks[i].capacityGB << L" GB";
        storageDisplay += s.str();
        if (disks[i].type == L"SSD" && disks[i].capacityGB >= TargetSSDGB) hasSuitableSSD = true;
    }
    if (storageDisplay.empty()) storageDisplay = L"Not Detected";
    std::wstring storageAction = hasSuitableSSD ? L"OK" : L"REPLACE WITH 256GB SSD";

    std::wcout << L"\n";
    std::wcout << L"============================================================\n";
    std::wcout << L"                 DEVICE INVENTORY REPORT\n";
    std::wcout << L"============================================================\n\n";
    std::wcout << L"Computer Name : " << computerName << L"\n";
    std::wcout << L"Manufacturer  : " << manufacturer << L"\n";
    std::wcout << L"Model        : " << model << L"\n";
    std::wcout << L"CPU          : " << cpuName << L"\n";
    std::wcout << L"RAM          : " << ramDisplay << L"\n";
    std::wcout << L"RAM Slots    : " << ramModules << L" / " << slotsTotal << L" (Used / Total)\n";
    std::wcout << L"Hard Disk    : " << storageDisplay << L"\n\n";
    std::wcout << L"============================================================\n";
    std::wcout << L"                 UPGRADE RECOMMENDATION\n";
    std::wcout << L"============================================================\n";
    std::wcout << L"RAM Upgrade     : " << ramAction << L"\n";
    std::wcout << L"Storage Upgrade : " << storageAction << L"\n\n";
    std::wcout << L"============================================================\n";

    wchar_t profile[MAX_PATH] = L"";
    DWORD profileLen = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    std::wstring outputFolder = profileLen ? profile : L".";
    outputFolder += L"\\Desktop\\Device_Report";

    CreateDirectoryW(outputFolder.c_str(), 0);
    std::wstring csvPath = outputFolder + L"\\DeviceInventory.csv";

    std::wstring diskField;
    for (size_t i = 0; i < disks.size(); ++i) {
        if (!diskField.empty()) diskField += L" | ";
        std::wostringstream s; s << disks[i].type << L" " << disks[i].capacityGB << L" GB";
        diskField += s.str();
    }
    if (diskField.empty()) diskField = L"Not Detected";
    
    csvFields.push_back(diskField);

std::vector<std::wstring> csvFields;

// Add computer information
csvFields.push_back(computerName);
csvFields.push_back(manufacturer);
csvFields.push_back(model);
csvFields.push_back(cpuName);

// Add RAM information
{ std::wostringstream s; s << totalRAMGB; csvFields.push_back(s.str()); }
csvFields.push_back(ramType);

{
    std::wostringstream s;
    s << ramModules << L" Used / " << slotsTotal << L" Total";
    csvFields.push_back(s.str());
}

{ std::wostringstream s; s << slotsFree; csvFields.push_back(s.str()); }

// Add disk information
csvFields.push_back(diskField);

// Save CSV
bool saved = WriteCsv(csvPath, csvFields);
    std::wcout << L"\n";
    if (saved) {
        std::wcout << L"Report saved to:\n" << csvPath << L"\n";
    } else {
        std::wcout << L"ERROR: Could not save the CSV report.\n" << csvPath << L"\n";
    }

    std::wcout << L"\nPress any key to exit...";
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD ir;
    DWORD count = 0;
    if (hIn != INVALID_HANDLE_VALUE) {
        while (ReadConsoleInputW(hIn, &ir, 1, &count)) {
            if (count && ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) break;
        }
    }

    CoUninitialize();
    return saved ? 0 : 2;
}
