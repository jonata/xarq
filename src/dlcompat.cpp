// dlcompat.cpp — see dlcompat.h. Windows only: "libfoo.so.N" is looked for as libfoo-N.dll, libfoo.dll, then
// any libfoo-*.dll beside the executable, the ways the same library is named among Windows builds.
#ifdef _WIN32
#include "dlcompat.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// The search leaves out the current directory and PATH: only the app's folder and the system's.
static HMODULE loadDll(const std::wstring& name){
    return LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

void* dlopen(const char* soname, int){
    const std::string so(soname);
    const size_t dot = so.find(".so");
    const std::wstring stem(so.begin(), so.begin()+(dot==std::string::npos ? so.size() : dot));
    if(dot!=std::string::npos && so.size()>dot+4){
        const std::wstring ver(so.begin()+dot+4, so.end());
        if(HMODULE h = loadDll(stem+L"-"+ver+L".dll")) return h;
    }
    if(HMODULE h = loadDll(stem+L".dll")) return h;

    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if(n==0 || n==MAX_PATH) return nullptr;
    std::wstring dir(exe, n);
    dir.resize(dir.find_last_of(L"\\/")+1);
    WIN32_FIND_DATAW found;
    HANDLE it = FindFirstFileW((dir+stem+L"-*.dll").c_str(), &found);
    if(it==INVALID_HANDLE_VALUE) return nullptr;
    HMODULE h = loadDll(dir+found.cFileName);
    FindClose(it);
    return h;
}

void* dlsym(void* lib, const char* symbol){
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), symbol));
}

int dlclose(void* lib){
    return FreeLibrary(static_cast<HMODULE>(lib)) ? 0 : 1;
}
#endif
