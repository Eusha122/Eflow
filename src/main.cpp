#include <windows.h>

#include <cwchar>

#include "App.h"
#include "core/PackageIdentity.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    // Start through the sparse package (if registered for this folder) so notifications can be read.
    if (eflow::RelaunchWithPackageIdentity(wcsstr(cmdLine, L"--packaged") != nullptr)) return 0;
    eflow::App app;
    return app.Run();
}
