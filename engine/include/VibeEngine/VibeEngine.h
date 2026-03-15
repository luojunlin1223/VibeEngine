#pragma once

// Convenience header — include this one file to get the full engine API
#include "Core/Log.h"
#include "Core/Window.h"
#include "Core/Application.h"
#include "Core/UUID.h"
#include "Core/Object.h"
#include "Core/FileDialog.h"
#include "Core/Console.h"
#include "Core/Profiler.h"
#include "Core/BuildExporter.h"

#include "Renderer/Renderer.h"
#include "Renderer/RenderCommand.h"
#include "Renderer/Buffer.h"
#include "Renderer/VertexArray.h"
#include "Renderer/Shader.h"
#include "Renderer/EditorCamera.h"
#include "Renderer/Texture.h"
#include "Renderer/Material.h"
#include "Renderer/Framebuffer.h"
#include "Renderer/PostProcessing.h"
#include "Renderer/GizmoRenderer.h"
#include "Renderer/SpriteBatchRenderer.h"
#include "Renderer/ParticleSystem.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/InstancedRenderer.h"
#include "Renderer/Frustum.h"
#include "Renderer/GridRenderer.h"
#include "Renderer/SSAO.h"
#include "Renderer/LODSystem.h"

#include "Navigation/NavGrid.h"

#include "Input/KeyCodes.h"
#include "Input/Input.h"
#include "Input/InputAction.h"

#include "Scene/Components.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"
#include "Scene/MeshLibrary.h"
#include "Scene/SceneSerializer.h"

#include "Asset/AssetDatabase.h"
#include "Asset/ThumbnailCache.h"
#include "Asset/MeshAsset.h"
#include "Asset/MeshImporter.h"

#include "Animation/Skeleton.h"
#include "Animation/AnimationClip.h"
#include "Animation/Animator.h"

#include "Scripting/NativeScript.h"
#include "Scripting/ScriptEngine.h"

#include "Audio/AudioEngine.h"

#include "UI/FontAtlas.h"
#include "UI/UIRenderer.h"

#include "Terrain/Terrain.h"

#include "Editor/EditorCommand.h"

#include "ImGui/ImGuiLayer.h"
#include <imgui.h>
