#include <windows.h>

// An old resident agent has an initializer but no protocol-version export.
extern "C" __declspec(dllexport) DWORD WINAPI HeapglassInitialize(LPVOID) {
    return ERROR_REVISION_MISMATCH;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
