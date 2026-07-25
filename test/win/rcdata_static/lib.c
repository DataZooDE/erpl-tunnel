/* Lives in the STATIC library, alongside the .res — this is the case under test:
 * does an RCDATA resource survive being linked in through a static archive? */
#include <windows.h>

int blob_size(void) {
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&blob_size, &m);
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (r == NULL) {
        return -1;
    }
    return (int)SizeofResource(m, r);
}
