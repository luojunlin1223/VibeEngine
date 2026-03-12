/*
 * ShaderLab parser and compiler implementation.
 *
 * Implements a recursive descent parser for Unity-style .shader files,
 * with GLSL extraction via VERTEX/FRAGMENT #ifdef blocks.
 */
#include "VibeEngine/Renderer/ShaderLab.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Core/Log.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace VE {

// ════════════════════════════════════════════════════════════════════
// Lexer
// ════════════════════════════════════════════════════════════════════

ShaderLabLexer::ShaderLabLexer(const std::string& source)
    : m_Source(source) {}

char ShaderLabLexer::Current() const {
    if (IsAtEnd()) return '\0';
    return m_Source[m_Pos];
}

char ShaderLabLexer::Advance() {
    char c = m_Source[m_Pos++];
    if (c == '\n') { m_Line++; m_Column = 1; }
    else { m_Column++; }
    return c;
}

bool ShaderLabLexer::IsAtEnd() const {
    return m_Pos >= m_Source.size();
}

void ShaderLabLexer::SkipWhitespace() {
    while (!IsAtEnd()) {
        char c = Current();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            Advance();
        } else if (c == '/' && m_Pos + 1 < m_Source.size()) {
            if (m_Source[m_Pos + 1] == '/') {
                SkipLineComment();
            } else if (m_Source[m_Pos + 1] == '*') {
                SkipBlockComment();
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

void ShaderLabLexer::SkipLineComment() {
    while (!IsAtEnd() && Current() != '\n') Advance();
}

void ShaderLabLexer::SkipBlockComment() {
    Advance(); // '/'
    Advance(); // '*'
    while (!IsAtEnd()) {
        if (Current() == '*' && m_Pos + 1 < m_Source.size() && m_Source[m_Pos + 1] == '/') {
            Advance(); Advance();
            return;
        }
        Advance();
    }
}

ShaderLabToken ShaderLabLexer::ReadString() {
    ShaderLabToken token;
    token.Type = ShaderLabTokenType::String;
    token.Line = m_Line;
    token.Column = m_Column;

    Advance(); // opening '"'
    std::string value;
    while (!IsAtEnd() && Current() != '"') {
        if (Current() == '\\' && m_Pos + 1 < m_Source.size()) {
            Advance();
            char escaped = Advance();
            switch (escaped) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                default: value += escaped; break;
            }
        } else {
            value += Advance();
        }
    }
    if (!IsAtEnd()) Advance(); // closing '"'
    token.Value = value;
    return token;
}

ShaderLabToken ShaderLabLexer::ReadNumber() {
    ShaderLabToken token;
    token.Type = ShaderLabTokenType::Number;
    token.Line = m_Line;
    token.Column = m_Column;

    std::string value;
    if (Current() == '-') value += Advance();
    while (!IsAtEnd() && (std::isdigit(Current()) || Current() == '.')) {
        value += Advance();
    }
    // Handle trailing 'f' suffix (e.g. 0.5f)
    if (!IsAtEnd() && (Current() == 'f' || Current() == 'F')) {
        Advance(); // skip 'f'
    }
    token.Value = value;
    return token;
}

ShaderLabToken ShaderLabLexer::ReadIdentifierOrKeyword() {
    ShaderLabToken token;
    token.Line = m_Line;
    token.Column = m_Column;

    std::string value;
    // Allow identifiers starting with _ or letter
    while (!IsAtEnd() && (std::isalnum(Current()) || Current() == '_')) {
        value += Advance();
    }

    // Check for keywords
    if (value == "Shader")       token.Type = ShaderLabTokenType::KW_Shader;
    else if (value == "Properties") token.Type = ShaderLabTokenType::KW_Properties;
    else if (value == "SubShader")  token.Type = ShaderLabTokenType::KW_SubShader;
    else if (value == "Pass")       token.Type = ShaderLabTokenType::KW_Pass;
    else if (value == "Tags")       token.Type = ShaderLabTokenType::KW_Tags;
    else if (value == "FallBack")   token.Type = ShaderLabTokenType::KW_FallBack;
    else if (value == "Name")       token.Type = ShaderLabTokenType::KW_Name;
    else if (value == "Cull")       token.Type = ShaderLabTokenType::KW_Cull;
    else if (value == "ZWrite")     token.Type = ShaderLabTokenType::KW_ZWrite;
    else if (value == "ZTest")      token.Type = ShaderLabTokenType::KW_ZTest;
    else if (value == "Blend")      token.Type = ShaderLabTokenType::KW_Blend;
    else if (value == "GLSLPROGRAM") token.Type = ShaderLabTokenType::KW_GLSLPROGRAM;
    else if (value == "ENDGLSL")    token.Type = ShaderLabTokenType::KW_ENDGLSL;
    else token.Type = ShaderLabTokenType::Identifier;

    token.Value = value;
    return token;
}

ShaderLabToken ShaderLabLexer::ReadGLSLBlock() {
    ShaderLabToken token;
    token.Type = ShaderLabTokenType::GLSLBlock;
    token.Line = m_Line;
    token.Column = m_Column;

    std::string glsl;
    while (!IsAtEnd()) {
        // Look for ENDGLSL
        if (Current() == 'E') {
            size_t savedPos = m_Pos;
            int savedLine = m_Line;
            int savedCol = m_Column;

            std::string word;
            while (!IsAtEnd() && std::isalpha(Current())) {
                word += Advance();
            }

            if (word == "ENDGLSL") {
                // Found the end marker — don't include it in the GLSL block
                token.Value = glsl;
                // Put back the cursor so the next NextToken() will read ENDGLSL
                // Actually we already consumed it, so we'll create a synthetic approach
                // We'll just return the block and let the parser handle ENDGLSL
                m_HasPeeked = true;
                m_PeekedToken.Type = ShaderLabTokenType::KW_ENDGLSL;
                m_PeekedToken.Value = "ENDGLSL";
                m_PeekedToken.Line = savedLine;
                m_PeekedToken.Column = savedCol;
                return token;
            } else {
                glsl += word;
            }
        } else {
            glsl += Advance();
        }
    }

    token.Value = glsl;
    return token;
}

ShaderLabToken ShaderLabLexer::NextToken() {
    if (m_HasPeeked) {
        m_HasPeeked = false;
        return m_PeekedToken;
    }

    SkipWhitespace();

    if (IsAtEnd()) {
        return { ShaderLabTokenType::EndOfFile, "", m_Line, m_Column };
    }

    char c = Current();

    switch (c) {
        case '{': { auto t = ShaderLabToken{ ShaderLabTokenType::LeftBrace, "{", m_Line, m_Column }; Advance(); return t; }
        case '}': { auto t = ShaderLabToken{ ShaderLabTokenType::RightBrace, "}", m_Line, m_Column }; Advance(); return t; }
        case '(': { auto t = ShaderLabToken{ ShaderLabTokenType::LeftParen, "(", m_Line, m_Column }; Advance(); return t; }
        case ')': { auto t = ShaderLabToken{ ShaderLabTokenType::RightParen, ")", m_Line, m_Column }; Advance(); return t; }
        case '=': { auto t = ShaderLabToken{ ShaderLabTokenType::Equals, "=", m_Line, m_Column }; Advance(); return t; }
        case ',': { auto t = ShaderLabToken{ ShaderLabTokenType::Comma, ",", m_Line, m_Column }; Advance(); return t; }
        case '"': return ReadString();
        default: break;
    }

    if (c == '-' || std::isdigit(c)) {
        // Check for special identifiers like "2D", "3D" that start with a digit
        if (std::isdigit(c) && m_Pos + 1 < m_Source.size() && std::isalpha(m_Source[m_Pos + 1])) {
            // This is an identifier like "2D" or "3D", not a number
            return ReadIdentifierOrKeyword();
        }
        // Disambiguate: negative number vs identifier
        if (c == '-') {
            if (m_Pos + 1 < m_Source.size() && std::isdigit(m_Source[m_Pos + 1])) {
                return ReadNumber();
            }
            // Otherwise it's just a minus sign; treat as part of identifier or error
        } else {
            return ReadNumber();
        }
    }

    if (std::isalpha(c) || c == '_') {
        return ReadIdentifierOrKeyword();
    }

    // Unknown character
    ShaderLabToken errToken;
    errToken.Type = ShaderLabTokenType::Error;
    errToken.Value = std::string(1, c);
    errToken.Line = m_Line;
    errToken.Column = m_Column;
    Advance();
    return errToken;
}

ShaderLabToken ShaderLabLexer::PeekToken() {
    if (!m_HasPeeked) {
        m_PeekedToken = NextToken();
        m_HasPeeked = true;
    }
    return m_PeekedToken;
}

// ════════════════════════════════════════════════════════════════════
// Parser
// ════════════════════════════════════════════════════════════════════

ShaderLabParser::ShaderLabParser(const std::string& source)
    : m_Lexer(source) {}

ShaderLabToken ShaderLabParser::Consume() {
    return m_Lexer.NextToken();
}

ShaderLabToken ShaderLabParser::Peek() {
    return m_Lexer.PeekToken();
}

bool ShaderLabParser::Check(ShaderLabTokenType type) {
    return Peek().Type == type;
}

bool ShaderLabParser::Expect(ShaderLabTokenType type, const std::string& context) {
    ShaderLabToken token = Consume();
    if (token.Type != type) {
        Error("Expected " + context + " at line " + std::to_string(token.Line)
              + ", got '" + token.Value + "'");
        return false;
    }
    return true;
}

void ShaderLabParser::Error(const std::string& msg) {
    m_Errors.push_back(msg);
}

bool ShaderLabParser::Parse(ShaderLabShader& outShader) {
    m_Errors.clear();
    return ParseShader(outShader);
}

bool ShaderLabParser::ParseFile(const std::string& filePath, ShaderLabShader& outShader,
                                 std::string* outError) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        if (outError) *outError = "Failed to open file: " + filePath;
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    ShaderLabParser parser(source);
    if (!parser.Parse(outShader)) {
        if (outError) {
            std::string errors;
            for (auto& e : parser.GetErrors()) {
                if (!errors.empty()) errors += "\n";
                errors += e;
            }
            *outError = errors;
        }
        return false;
    }
    return true;
}

bool ShaderLabParser::ParseShader(ShaderLabShader& shader) {
    // Shader "Name" {
    if (!Expect(ShaderLabTokenType::KW_Shader, "'Shader' keyword")) return false;

    ShaderLabToken nameToken = Consume();
    if (nameToken.Type != ShaderLabTokenType::String) {
        Error("Expected shader name string at line " + std::to_string(nameToken.Line));
        return false;
    }
    shader.Name = nameToken.Value;

    if (!Expect(ShaderLabTokenType::LeftBrace, "'{'")) return false;

    // Parse contents: Properties, SubShader, FallBack
    while (!Check(ShaderLabTokenType::RightBrace) && !Check(ShaderLabTokenType::EndOfFile)) {
        ShaderLabToken peek = Peek();

        if (peek.Type == ShaderLabTokenType::KW_Properties) {
            Consume();
            if (!ParseProperties(shader.Properties)) return false;
        } else if (peek.Type == ShaderLabTokenType::KW_SubShader) {
            Consume();
            ShaderLabSubShader subShader;
            if (!ParseSubShader(subShader)) return false;
            shader.SubShaders.push_back(std::move(subShader));
        } else if (peek.Type == ShaderLabTokenType::KW_FallBack) {
            Consume();
            ShaderLabToken fbToken = Consume();
            if (fbToken.Type == ShaderLabTokenType::String) {
                shader.FallBack = fbToken.Value;
            } else if (fbToken.Type == ShaderLabTokenType::Identifier && fbToken.Value == "Off") {
                shader.FallBack = "Off";
            } else {
                Error("Expected FallBack shader name or 'Off' at line " + std::to_string(fbToken.Line));
                return false;
            }
        } else {
            Error("Unexpected token '" + peek.Value + "' at line " + std::to_string(peek.Line));
            return false;
        }
    }

    if (!Expect(ShaderLabTokenType::RightBrace, "'}'")) return false;
    return m_Errors.empty();
}

bool ShaderLabParser::ParseProperties(std::vector<ShaderLabProperty>& props) {
    if (!Expect(ShaderLabTokenType::LeftBrace, "'{'")) return false;

    while (!Check(ShaderLabTokenType::RightBrace) && !Check(ShaderLabTokenType::EndOfFile)) {
        // Property names start with _ or letter
        if (Check(ShaderLabTokenType::Identifier)) {
            ShaderLabProperty prop;
            if (!ParseProperty(prop)) return false;
            props.push_back(std::move(prop));
        } else {
            ShaderLabToken t = Peek();
            Error("Unexpected token '" + t.Value + "' in Properties at line " + std::to_string(t.Line));
            return false;
        }
    }

    if (!Expect(ShaderLabTokenType::RightBrace, "'}'")) return false;
    return true;
}

bool ShaderLabParser::ParseProperty(ShaderLabProperty& prop) {
    // _Name ("Display Name", Type) = DefaultValue
    ShaderLabToken nameToken = Consume();
    prop.Name = nameToken.Value;

    if (!Expect(ShaderLabTokenType::LeftParen, "'('")) return false;

    // Display name
    ShaderLabToken displayToken = Consume();
    if (displayToken.Type != ShaderLabTokenType::String) {
        Error("Expected display name string at line " + std::to_string(displayToken.Line));
        return false;
    }
    prop.DisplayName = displayToken.Value;

    if (!Expect(ShaderLabTokenType::Comma, "','")) return false;

    // Type
    ShaderLabToken typeToken = Consume();
    std::string typeStr = typeToken.Value;

    if (typeStr == "Float") {
        prop.Type = ShaderLabPropertyType::Float;
    } else if (typeStr == "Int") {
        prop.Type = ShaderLabPropertyType::Int;
    } else if (typeStr == "Color") {
        prop.Type = ShaderLabPropertyType::Color;
    } else if (typeStr == "Vector") {
        prop.Type = ShaderLabPropertyType::Vector;
    } else if (typeStr == "2D") {
        prop.Type = ShaderLabPropertyType::Texture2D;
    } else if (typeStr == "3D") {
        prop.Type = ShaderLabPropertyType::Texture3D;
    } else if (typeStr == "Cube") {
        prop.Type = ShaderLabPropertyType::Cube;
    } else if (typeStr == "Range") {
        prop.Type = ShaderLabPropertyType::Range;
        // Range(min, max)
        if (!Expect(ShaderLabTokenType::LeftParen, "'('")) return false;
        ShaderLabToken minToken = Consume();
        prop.RangeMin = std::stof(minToken.Value);
        if (!Expect(ShaderLabTokenType::Comma, "','")) return false;
        ShaderLabToken maxToken = Consume();
        prop.RangeMax = std::stof(maxToken.Value);
        if (!Expect(ShaderLabTokenType::RightParen, "')'")) return false;
    } else {
        Error("Unknown property type '" + typeStr + "' at line " + std::to_string(typeToken.Line));
        return false;
    }

    if (!Expect(ShaderLabTokenType::RightParen, "')'")) return false;
    if (!Expect(ShaderLabTokenType::Equals, "'='")) return false;

    // Default value
    switch (prop.Type) {
        case ShaderLabPropertyType::Float:
        case ShaderLabPropertyType::Range: {
            ShaderLabToken valToken = Consume();
            prop.FloatDefault = std::stof(valToken.Value);
            break;
        }
        case ShaderLabPropertyType::Int: {
            ShaderLabToken valToken = Consume();
            prop.IntDefault = std::stoi(valToken.Value);
            break;
        }
        case ShaderLabPropertyType::Color:
        case ShaderLabPropertyType::Vector: {
            // (r, g, b, a)
            if (!Expect(ShaderLabTokenType::LeftParen, "'('")) return false;
            ShaderLabToken x = Consume(); prop.VectorDefault.x = std::stof(x.Value);
            if (!Expect(ShaderLabTokenType::Comma, "','")) return false;
            ShaderLabToken y = Consume(); prop.VectorDefault.y = std::stof(y.Value);
            if (!Expect(ShaderLabTokenType::Comma, "','")) return false;
            ShaderLabToken z = Consume(); prop.VectorDefault.z = std::stof(z.Value);
            if (!Expect(ShaderLabTokenType::Comma, "','")) return false;
            ShaderLabToken w = Consume(); prop.VectorDefault.w = std::stof(w.Value);
            if (!Expect(ShaderLabTokenType::RightParen, "')'")) return false;
            break;
        }
        case ShaderLabPropertyType::Texture2D:
        case ShaderLabPropertyType::Texture3D:
        case ShaderLabPropertyType::Cube: {
            // "white" {}
            ShaderLabToken texDefault = Consume();
            if (texDefault.Type == ShaderLabTokenType::String) {
                prop.TextureDefault = texDefault.Value;
            }
            if (!Expect(ShaderLabTokenType::LeftBrace, "'{'")) return false;
            if (!Expect(ShaderLabTokenType::RightBrace, "'}'")) return false;
            break;
        }
    }

    return true;
}

bool ShaderLabParser::ParseSubShader(ShaderLabSubShader& subShader) {
    if (!Expect(ShaderLabTokenType::LeftBrace, "'{'")) return false;

    while (!Check(ShaderLabTokenType::RightBrace) && !Check(ShaderLabTokenType::EndOfFile)) {
        ShaderLabToken peek = Peek();

        if (peek.Type == ShaderLabTokenType::KW_Tags) {
            Consume();
            if (!ParseTags(subShader.Tags)) return false;
        } else if (peek.Type == ShaderLabTokenType::KW_Pass) {
            Consume();
            ShaderLabPass pass;
            if (!ParsePass(pass)) return false;
            subShader.Passes.push_back(std::move(pass));
        } else {
            Error("Unexpected token '" + peek.Value + "' in SubShader at line " + std::to_string(peek.Line));
            return false;
        }
    }

    if (!Expect(ShaderLabTokenType::RightBrace, "'}'")) return false;
    return true;
}

bool ShaderLabParser::ParseTags(std::unordered_map<std::string, std::string>& tags) {
    if (!Expect(ShaderLabTokenType::LeftBrace, "'{'")) return false;

    while (!Check(ShaderLabTokenType::RightBrace) && !Check(ShaderLabTokenType::EndOfFile)) {
        // "Key" = "Value"
        ShaderLabToken keyToken = Consume();
        if (keyToken.Type != ShaderLabTokenType::String) {
            Error("Expected tag key string at line " + std::to_string(keyToken.Line));
            return false;
        }
        if (!Expect(ShaderLabTokenType::Equals, "'='")) return false;
        ShaderLabToken valToken = Consume();
        if (valToken.Type != ShaderLabTokenType::String) {
            Error("Expected tag value string at line " + std::to_string(valToken.Line));
            return false;
        }
        tags[keyToken.Value] = valToken.Value;
    }

    if (!Expect(ShaderLabTokenType::RightBrace, "'}'")) return false;
    return true;
}

bool ShaderLabParser::ParsePass(ShaderLabPass& pass) {
    if (!Expect(ShaderLabTokenType::LeftBrace, "'{'")) return false;

    while (!Check(ShaderLabTokenType::RightBrace) && !Check(ShaderLabTokenType::EndOfFile)) {
        ShaderLabToken peek = Peek();

        if (peek.Type == ShaderLabTokenType::KW_Name) {
            Consume();
            ShaderLabToken nameToken = Consume();
            if (nameToken.Type != ShaderLabTokenType::String) {
                Error("Expected pass name string at line " + std::to_string(nameToken.Line));
                return false;
            }
            pass.Name = nameToken.Value;
        } else if (peek.Type == ShaderLabTokenType::KW_Tags) {
            Consume();
            if (!ParseTags(pass.Tags)) return false;
        } else if (peek.Type == ShaderLabTokenType::KW_Cull ||
                   peek.Type == ShaderLabTokenType::KW_ZWrite ||
                   peek.Type == ShaderLabTokenType::KW_ZTest ||
                   peek.Type == ShaderLabTokenType::KW_Blend) {
            if (!ParseRenderState(pass)) return false;
        } else if (peek.Type == ShaderLabTokenType::KW_GLSLPROGRAM) {
            if (!ParseGLSLBlock(pass)) return false;
        } else {
            Error("Unexpected token '" + peek.Value + "' in Pass at line " + std::to_string(peek.Line));
            return false;
        }
    }

    if (!Expect(ShaderLabTokenType::RightBrace, "'}'")) return false;
    return true;
}

bool ShaderLabParser::ParseRenderState(ShaderLabPass& pass) {
    ShaderLabToken keyword = Consume();

    if (keyword.Type == ShaderLabTokenType::KW_Cull) {
        ShaderLabToken val = Consume();
        if (val.Value == "Back")       pass.RenderState.Cull = CullMode::Back;
        else if (val.Value == "Front") pass.RenderState.Cull = CullMode::Front;
        else if (val.Value == "Off")   pass.RenderState.Cull = CullMode::Off;
        else { Error("Invalid Cull mode '" + val.Value + "' at line " + std::to_string(val.Line)); return false; }
    } else if (keyword.Type == ShaderLabTokenType::KW_ZWrite) {
        ShaderLabToken val = Consume();
        if (val.Value == "On")       pass.RenderState.ZWrite = ZWriteMode::On;
        else if (val.Value == "Off") pass.RenderState.ZWrite = ZWriteMode::Off;
        else { Error("Invalid ZWrite mode '" + val.Value + "' at line " + std::to_string(val.Line)); return false; }
    } else if (keyword.Type == ShaderLabTokenType::KW_ZTest) {
        ShaderLabToken val = Consume();
        if (val.Value == "Less")            pass.RenderState.ZTest = ZTestFunc::Less;
        else if (val.Value == "LEqual")     pass.RenderState.ZTest = ZTestFunc::LEqual;
        else if (val.Value == "Equal")      pass.RenderState.ZTest = ZTestFunc::Equal;
        else if (val.Value == "GEqual")     pass.RenderState.ZTest = ZTestFunc::GEqual;
        else if (val.Value == "Greater")    pass.RenderState.ZTest = ZTestFunc::Greater;
        else if (val.Value == "NotEqual")   pass.RenderState.ZTest = ZTestFunc::NotEqual;
        else if (val.Value == "Always")     pass.RenderState.ZTest = ZTestFunc::Always;
        else if (val.Value == "Never")      pass.RenderState.ZTest = ZTestFunc::Never;
        else { Error("Invalid ZTest func '" + val.Value + "' at line " + std::to_string(val.Line)); return false; }
    } else if (keyword.Type == ShaderLabTokenType::KW_Blend) {
        pass.RenderState.BlendEnabled = true;
        ShaderLabToken src = Consume();
        ShaderLabToken dst = Consume();
        auto parseBlendFactor = [](const std::string& s) -> BlendFactor {
            if (s == "One")                return BlendFactor::One;
            if (s == "Zero")               return BlendFactor::Zero;
            if (s == "SrcColor")           return BlendFactor::SrcColor;
            if (s == "SrcAlpha")           return BlendFactor::SrcAlpha;
            if (s == "DstColor")           return BlendFactor::DstColor;
            if (s == "DstAlpha")           return BlendFactor::DstAlpha;
            if (s == "OneMinusSrcColor")   return BlendFactor::OneMinusSrcColor;
            if (s == "OneMinusSrcAlpha")   return BlendFactor::OneMinusSrcAlpha;
            if (s == "OneMinusDstColor")   return BlendFactor::OneMinusDstColor;
            if (s == "OneMinusDstAlpha")   return BlendFactor::OneMinusDstAlpha;
            return BlendFactor::One;
        };
        pass.RenderState.BlendSrc = parseBlendFactor(src.Value);
        pass.RenderState.BlendDst = parseBlendFactor(dst.Value);
    }

    return true;
}

bool ShaderLabParser::ParseGLSLBlock(ShaderLabPass& pass) {
    Consume(); // consume GLSLPROGRAM

    // Read the raw GLSL block until ENDGLSL
    ShaderLabToken glslToken = m_Lexer.ReadGLSLBlock();
    pass.RawGLSL = glslToken.Value;

    // Consume ENDGLSL (may be peeked by ReadGLSLBlock)
    if (Check(ShaderLabTokenType::KW_ENDGLSL)) {
        Consume();
    }

    // Extract vertex and fragment sources
    ShaderLabCompiler::ExtractGLSL(pass.RawGLSL, pass.VertexEntry, pass.FragmentEntry,
                                   pass.VertexSource, pass.FragmentSource);

    return true;
}

// ════════════════════════════════════════════════════════════════════
// Compiler
// ════════════════════════════════════════════════════════════════════

bool ShaderLabCompiler::ExtractGLSL(const std::string& rawGLSL,
                                     const std::string& /*vertexEntry*/,
                                     const std::string& /*fragmentEntry*/,
                                     std::string& outVertexSrc,
                                     std::string& outFragmentSrc) {
    // Strategy: look for #ifdef VERTEX ... #endif and #ifdef FRAGMENT ... #endif blocks
    // If not found, look for #pragma vertex / #pragma fragment markers

    // First, parse #pragma directives to extract version and shared code
    std::string versionLine;
    std::string sharedCode;

    std::istringstream stream(rawGLSL);
    std::string line;
    std::string vertexCode;
    std::string fragmentCode;

    enum class Section { None, Vertex, Fragment };
    Section currentSection = Section::None;
    int ifdefDepth = 0;

    while (std::getline(stream, line)) {
        // Trim leading whitespace for directive detection
        std::string trimmed = line;
        size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos) {
            trimmed = trimmed.substr(firstNonSpace);
        } else {
            trimmed = "";
        }

        // Skip #pragma vertex/fragment lines
        if (trimmed.find("#pragma vertex") == 0 || trimmed.find("#pragma fragment") == 0) {
            continue;
        }

        // Detect #ifdef VERTEX / #ifdef FRAGMENT
        if (trimmed.find("#ifdef VERTEX") == 0 || trimmed.find("#ifdef  VERTEX") == 0) {
            currentSection = Section::Vertex;
            ifdefDepth = 1;
            continue;
        }
        if (trimmed.find("#ifdef FRAGMENT") == 0 || trimmed.find("#ifdef  FRAGMENT") == 0) {
            currentSection = Section::Fragment;
            ifdefDepth = 1;
            continue;
        }

        // Track nested #ifdef/#endif
        if (currentSection != Section::None) {
            if (trimmed.find("#ifdef") == 0 || trimmed.find("#ifndef") == 0 || trimmed.find("#if ") == 0) {
                ifdefDepth++;
            }
            if (trimmed.find("#endif") == 0) {
                ifdefDepth--;
                if (ifdefDepth == 0) {
                    currentSection = Section::None;
                    continue;
                }
            }
        }

        // Route line to appropriate section
        switch (currentSection) {
            case Section::Vertex:
                vertexCode += line + "\n";
                break;
            case Section::Fragment:
                fragmentCode += line + "\n";
                break;
            case Section::None:
                // Shared code (version, common declarations)
                sharedCode += line + "\n";
                break;
        }
    }

    if (vertexCode.empty() || fragmentCode.empty()) {
        VE_ENGINE_ERROR("ShaderLab: Failed to extract VERTEX/FRAGMENT sections from GLSL block");
        return false;
    }

    outVertexSrc = sharedCode + vertexCode;
    outFragmentSrc = sharedCode + fragmentCode;
    return true;
}

std::shared_ptr<Shader> ShaderLabCompiler::Compile(const ShaderLabShader& shader) {
    if (shader.SubShaders.empty()) {
        VE_ENGINE_ERROR("ShaderLab: Shader '{}' has no SubShaders", shader.Name);
        return nullptr;
    }

    const auto& subShader = shader.SubShaders[0];
    if (subShader.Passes.empty()) {
        VE_ENGINE_ERROR("ShaderLab: Shader '{}' SubShader has no Passes", shader.Name);
        return nullptr;
    }

    const auto& pass = subShader.Passes[0];

    if (pass.VertexSource.empty() || pass.FragmentSource.empty()) {
        VE_ENGINE_ERROR("ShaderLab: Shader '{}' has empty vertex or fragment source", shader.Name);
        return nullptr;
    }

    auto result = Shader::Create(pass.VertexSource, pass.FragmentSource);
    if (result) {
        result->SetName(shader.Name);

        // Propagate ShaderLab Properties to Shader metadata
        std::vector<ShaderPropertyInfo> infos;
        for (const auto& prop : shader.Properties) {
            ShaderPropertyInfo info;
            // Convert ShaderLab name (_Foo) to uniform name (u_Foo)
            if (prop.Name.size() > 1 && prop.Name[0] == '_')
                info.Name = "u_" + prop.Name.substr(1);
            else
                info.Name = prop.Name;
            info.DisplayName = prop.DisplayName;
            info.FloatDefault = prop.FloatDefault;
            info.IntDefault = prop.IntDefault;
            info.VectorDefault = prop.VectorDefault;
            info.TextureDefault = prop.TextureDefault;
            info.RangeMin = prop.RangeMin;
            info.RangeMax = prop.RangeMax;

            switch (prop.Type) {
                case ShaderLabPropertyType::Float:   info.Type = ShaderPropertyType::Float;     break;
                case ShaderLabPropertyType::Range:   info.Type = ShaderPropertyType::Range;     break;
                case ShaderLabPropertyType::Int:     info.Type = ShaderPropertyType::Int;       break;
                case ShaderLabPropertyType::Color:   info.Type = ShaderPropertyType::Color;     break;
                case ShaderLabPropertyType::Vector:  info.Type = ShaderPropertyType::Vector;    break;
                case ShaderLabPropertyType::Texture2D: info.Type = ShaderPropertyType::Texture2D; break;
                default: info.Type = ShaderPropertyType::Float; break;
            }
            infos.push_back(std::move(info));
        }
        result->SetPropertyInfos(std::move(infos));

        VE_ENGINE_INFO("ShaderLab: Compiled shader '{}' ({} properties)", shader.Name, shader.Properties.size());
    }
    return result;
}

std::shared_ptr<Shader> ShaderLabCompiler::CompileFile(const std::string& filePath) {
    ShaderLabShader shader;
    std::string error;
    if (!ShaderLabParser::ParseFile(filePath, shader, &error)) {
        VE_ENGINE_ERROR("ShaderLab: Failed to parse '{}': {}", filePath, error);
        return nullptr;
    }
    return Compile(shader);
}

ShaderLabRenderState ShaderLabCompiler::GetRenderState(const ShaderLabShader& shader) {
    if (!shader.SubShaders.empty() && !shader.SubShaders[0].Passes.empty()) {
        return shader.SubShaders[0].Passes[0].RenderState;
    }
    return {};
}

const std::vector<ShaderLabProperty>& ShaderLabCompiler::GetProperties(const ShaderLabShader& shader) {
    return shader.Properties;
}

} // namespace VE
