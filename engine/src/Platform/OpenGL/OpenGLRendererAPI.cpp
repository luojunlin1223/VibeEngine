#include "VibeEngine/Platform/OpenGL/OpenGLRendererAPI.h"
#include "VibeEngine/Renderer/VertexArray.h"

#include <glad/gl.h>

namespace VE {

void OpenGLRendererAPI::Init() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
}

void OpenGLRendererAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void OpenGLRendererAPI::SetClearColor(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
}

void OpenGLRendererAPI::Clear() {
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glStencilMask(0x00);
}

void OpenGLRendererAPI::DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray) {
    vertexArray->Bind();
    glDrawElements(GL_TRIANGLES,
                   static_cast<GLsizei>(vertexArray->GetIndexBuffer()->GetCount()),
                   GL_UNSIGNED_INT, nullptr);
}

void OpenGLRendererAPI::DrawLines(const std::shared_ptr<VertexArray>& vertexArray, uint32_t vertexCount) {
    vertexArray->Bind();
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertexCount));
}

void OpenGLRendererAPI::SetLineWidth(float width) {
    glLineWidth(width);
}

void OpenGLRendererAPI::SetDepthFunc(DepthFunc func) {
    switch (func) {
        case DepthFunc::Less:      glDepthFunc(GL_LESS);   break;
        case DepthFunc::LessEqual: glDepthFunc(GL_LEQUAL); break;
    }
}

void OpenGLRendererAPI::SetDepthWrite(bool enabled) {
    glDepthMask(enabled ? GL_TRUE : GL_FALSE);
}

} // namespace VE
