// dlcompat.h — dlopen/dlsym/dlclose for the optional libraries loaded at runtime. POSIX has them; on Windows
// dlcompat.cpp provides them over LoadLibrary, and takes the same "libfoo.so.N" names the callers ask for.
#pragma once
#ifndef _WIN32
#include <dlfcn.h>
#else
#define RTLD_NOW 0
#define RTLD_LOCAL 0
void* dlopen(const char* soname, int flags);
void* dlsym(void* lib, const char* symbol);
int dlclose(void* lib);
#endif
