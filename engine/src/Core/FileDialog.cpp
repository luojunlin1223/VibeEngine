#include "VibeEngine/Core/FileDialog.h"

#ifdef VE_PLATFORM_WINDOWS

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <commdlg.h>
#include <string>

namespace VE {

std::string FileDialog::OpenFile(const char* filter, GLFWwindow* window) {
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(window);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn))
        return std::string(szFile);

    return {};
}

std::string FileDialog::SaveFile(const char* filter, GLFWwindow* window) {
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(window);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrDefExt = "vscene";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameA(&ofn))
        return std::string(szFile);

    return {};
}

} // namespace VE

#else

// Stub for non-Windows platforms
namespace VE {
std::string FileDialog::OpenFile(const char*, GLFWwindow*) { return {}; }
std::string FileDialog::SaveFile(const char*, GLFWwindow*) { return {}; }
} // namespace VE

#endif
