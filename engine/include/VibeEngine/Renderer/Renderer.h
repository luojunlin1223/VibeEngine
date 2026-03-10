#pragma once

#include "RendererAPI.h"

namespace VE {

class Renderer {
public:
    static void Init(RendererAPI::API api = RendererAPI::API::OpenGL);
    static void Shutdown();
    static void BeginFrame();
    static void EndFrame();

    static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
};

} // namespace VE
