/*
 * FileDialog — Native OS file open/save dialogs.
 *
 * Windows implementation uses GetOpenFileName / GetSaveFileName.
 */
#pragma once

#include <string>

struct GLFWwindow;

namespace VE {

class FileDialog {
public:
    static std::string OpenFile(const char* filter, GLFWwindow* window);
    static std::string SaveFile(const char* filter, GLFWwindow* window);
};

} // namespace VE
