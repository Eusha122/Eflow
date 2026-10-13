#include <windows.h>

#include "App.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    eflow::App app;
    return app.Run();
}
