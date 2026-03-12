/*
 * ShaderLab — Unity-style shader definition language for VibeEngine.
 *
 * Provides a parser and compiler for .shader files that follow Unity's ShaderLab
 * syntax. This allows shader authoring with a unified syntax that can target
 * multiple backends (OpenGL, Vulkan, etc.) in the future.
 *
 * Syntax overview:
 *   Shader "Name" {
 *       Properties {
 *           _PropName ("Display Name", Type) = DefaultValue
 *       }
 *       SubShader {
 *           Tags { "Key"="Value" }
 *           Pass {
 *               Name "PassName"
 *               Cull Back|Front|Off
 *               ZWrite On|Off
 *               ZTest Less|LEqual|Equal|GEqual|Greater|NotEqual|Always
 *               Blend SrcFactor DstFactor
 *
 *               GLSLPROGRAM
 *               #pragma vertex vert
 *               #pragma fragment frag
 *               ... GLSL code ...
 *               ENDGLSL
 *           }
 *       }
 *       FallBack "ShaderName" | FallBack Off
 *   }
 */
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>

namespace VE {

class Shader;

// ── Property Types ──────────────────────────────────────────────────

enum class ShaderLabPropertyType {
    Float,      // _Name ("Display", Float) = 0.5
    Range,      // _Name ("Display", Range(0, 1)) = 0.5
    Int,        // _Name ("Display", Int) = 1
    Color,      // _Name ("Display", Color) = (1,1,1,1)
    Vector,     // _Name ("Display", Vector) = (0,0,0,0)
    Texture2D,  // _Name ("Display", 2D) = "white" {}
    Texture3D,  // _Name ("Display", 3D) = "" {}
    Cube,       // _Name ("Display", Cube) = "" {}
};

struct ShaderLabProperty {
    std::string Name;           // e.g. "_MainTex"
    std::string DisplayName;    // e.g. "Main Texture"
    ShaderLabPropertyType Type = ShaderLabPropertyType::Float;

    // Default values
    float FloatDefault = 0.0f;
    int   IntDefault   = 0;
    glm::vec4 VectorDefault = glm::vec4(0.0f);
    std::string TextureDefault;  // "white", "black", "normal", "bump", ""

    // Range constraints
    float RangeMin = 0.0f;
    float RangeMax = 1.0f;
};

// ── Render State ────────────────────────────────────────────────────

enum class CullMode    { Back, Front, Off };
enum class ZWriteMode  { On, Off };
enum class ZTestFunc   { Less, LEqual, Equal, GEqual, Greater, NotEqual, Always, Never };
enum class BlendFactor { One, Zero, SrcColor, SrcAlpha, DstColor, DstAlpha,
                         OneMinusSrcColor, OneMinusSrcAlpha, OneMinusDstColor, OneMinusDstAlpha };

struct ShaderLabRenderState {
    CullMode   Cull   = CullMode::Back;
    ZWriteMode ZWrite = ZWriteMode::On;
    ZTestFunc  ZTest  = ZTestFunc::LEqual;

    bool BlendEnabled = false;
    BlendFactor BlendSrc = BlendFactor::One;
    BlendFactor BlendDst = BlendFactor::Zero;
};

// ── Pass ────────────────────────────────────────────────────────────

struct ShaderLabPass {
    std::string Name;
    std::unordered_map<std::string, std::string> Tags;
    ShaderLabRenderState RenderState;

    // Extracted GLSL source (from GLSLPROGRAM...ENDGLSL block)
    std::string VertexSource;
    std::string FragmentSource;

    // Raw GLSL block (before splitting by #pragma)
    std::string RawGLSL;

    // Pragma directives
    std::string VertexEntry = "vert";
    std::string FragmentEntry = "frag";
};

// ── SubShader ───────────────────────────────────────────────────────

struct ShaderLabSubShader {
    std::unordered_map<std::string, std::string> Tags;
    std::vector<ShaderLabPass> Passes;
};

// ── Shader (top-level AST) ──────────────────────────────────────────

struct ShaderLabShader {
    std::string Name;
    std::vector<ShaderLabProperty> Properties;
    std::vector<ShaderLabSubShader> SubShaders;
    std::string FallBack;   // empty string or "Off" means no fallback
};

// ── Token types for lexer ───────────────────────────────────────────

enum class ShaderLabTokenType {
    // Literals
    Identifier,
    String,         // "quoted string"
    Number,         // 0.5, 42, -1.0
    // Symbols
    LeftBrace,      // {
    RightBrace,     // }
    LeftParen,      // (
    RightParen,     // )
    Equals,         // =
    Comma,          // ,
    // Keywords
    KW_Shader,
    KW_Properties,
    KW_SubShader,
    KW_Pass,
    KW_Tags,
    KW_FallBack,
    KW_Name,
    KW_Cull,
    KW_ZWrite,
    KW_ZTest,
    KW_Blend,
    KW_GLSLPROGRAM,
    KW_ENDGLSL,
    // Special
    GLSLBlock,      // Raw GLSL source between GLSLPROGRAM and ENDGLSL
    EndOfFile,
    Error,
};

struct ShaderLabToken {
    ShaderLabTokenType Type = ShaderLabTokenType::Error;
    std::string Value;
    int Line = 0;
    int Column = 0;
};

// ── Lexer ───────────────────────────────────────────────────────────

class ShaderLabLexer {
public:
    explicit ShaderLabLexer(const std::string& source);

    ShaderLabToken NextToken();
    ShaderLabToken PeekToken();

    int GetLine() const { return m_Line; }
    int GetColumn() const { return m_Column; }

    /// Read raw GLSL block until ENDGLSL marker (used by parser)
    ShaderLabToken ReadGLSLBlock();

private:
    void SkipWhitespace();
    void SkipLineComment();
    void SkipBlockComment();
    ShaderLabToken ReadString();
    ShaderLabToken ReadNumber();
    ShaderLabToken ReadIdentifierOrKeyword();
    char Current() const;
    char Advance();
    bool IsAtEnd() const;

    std::string m_Source;
    size_t m_Pos = 0;
    int m_Line = 1;
    int m_Column = 1;
    bool m_HasPeeked = false;
    ShaderLabToken m_PeekedToken;
};

// ── Parser ──────────────────────────────────────────────────────────

class ShaderLabParser {
public:
    explicit ShaderLabParser(const std::string& source);

    /// Parse a .shader file and return the AST. Returns true on success.
    bool Parse(ShaderLabShader& outShader);

    /// Get parse errors
    const std::vector<std::string>& GetErrors() const { return m_Errors; }

    /// Parse a .shader file from disk
    static bool ParseFile(const std::string& filePath, ShaderLabShader& outShader,
                          std::string* outError = nullptr);

private:
    bool ParseShader(ShaderLabShader& shader);
    bool ParseProperties(std::vector<ShaderLabProperty>& props);
    bool ParseProperty(ShaderLabProperty& prop);
    bool ParseSubShader(ShaderLabSubShader& subShader);
    bool ParsePass(ShaderLabPass& pass);
    bool ParseTags(std::unordered_map<std::string, std::string>& tags);
    bool ParseRenderState(ShaderLabPass& pass);
    bool ParseGLSLBlock(ShaderLabPass& pass);

    bool Expect(ShaderLabTokenType type, const std::string& context);
    ShaderLabToken Consume();
    ShaderLabToken Peek();
    bool Check(ShaderLabTokenType type);
    void Error(const std::string& msg);

    ShaderLabLexer m_Lexer;
    std::vector<std::string> m_Errors;
};

// ── Compiler ────────────────────────────────────────────────────────

class ShaderLabCompiler {
public:
    /// Compile a ShaderLabShader AST into a Shader object.
    /// Uses the first SubShader's first Pass for now.
    static std::shared_ptr<Shader> Compile(const ShaderLabShader& shader);

    /// Load and compile a .shader file from disk.
    static std::shared_ptr<Shader> CompileFile(const std::string& filePath);

    /// Extract vertex and fragment GLSL source from raw GLSL block.
    /// Splits by VERTEX/FRAGMENT #ifdef sections or by #pragma vertex/fragment.
    static bool ExtractGLSL(const std::string& rawGLSL,
                            const std::string& vertexEntry,
                            const std::string& fragmentEntry,
                            std::string& outVertexSrc,
                            std::string& outFragmentSrc);

    /// Get the render state from the first Pass of the first SubShader.
    static ShaderLabRenderState GetRenderState(const ShaderLabShader& shader);

    /// Get properties from the shader.
    static const std::vector<ShaderLabProperty>& GetProperties(const ShaderLabShader& shader);
};

} // namespace VE
