/*
 * EditorCommand — Command Pattern for editor undo/redo.
 *
 * Every editor operation (create entity, modify property, etc.) is
 * encapsulated as a command object. CommandHistory manages the undo/redo
 * stacks and uses scene YAML snapshots for state restoration.
 */
#pragma once

#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/SceneSerializer.h"

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace VE {

// ── Base command ──────────────────────────────────────────────────────

class EditorCommand {
public:
    virtual ~EditorCommand() = default;
    virtual const char* GetName() const = 0;

    // Scene snapshots managed by CommandHistory
    std::string UndoSnapshot;   // scene state before the operation
    std::string RedoSnapshot;   // scene state after the operation
    uint64_t UndoSelectedUUID = 0;
    uint64_t RedoSelectedUUID = 0;
};

// ── Structural action (create/delete entity, add/remove component, etc.) ──

class EditorAction : public EditorCommand {
public:
    EditorAction(std::string name, std::function<void()> action)
        : m_Name(std::move(name)), m_Action(std::move(action)) {}

    const char* GetName() const override { return m_Name.c_str(); }
    void Execute() { if (m_Action) m_Action(); }

private:
    std::string m_Name;
    std::function<void()> m_Action;
};

// ── Property edit (already applied by ImGui widget) ──────────────────

class PropertyEditAction : public EditorCommand {
public:
    PropertyEditAction(std::string name)
        : m_Name(std::move(name)) {}

    const char* GetName() const override { return m_Name.c_str(); }

private:
    std::string m_Name;
};

// ── Command History ──────────────────────────────────────────────────

class CommandHistory {
public:
    static constexpr int MAX_LEVELS = 50;

    void SetScene(std::shared_ptr<Scene>* scenePtr) { m_ScenePtr = scenePtr; }

    using RestoreCallback = std::function<void()>;
    void SetRestoreCallback(RestoreCallback cb) { m_RestoreCallback = std::move(cb); }

    using DirtyCallback = std::function<void()>;
    void SetDirtyCallback(DirtyCallback cb) { m_DirtyCallback = std::move(cb); }

    // ── Structural operations ──
    // Captures before-snapshot, executes action, captures after-snapshot.
    void Execute(const std::string& name, std::function<void()> action);

    // ── Property edit support ──
    // Call on ImGui::IsItemActivated — captures scene before edit starts.
    void BeginPropertyEdit(const std::string& name);
    // Call on ImGui::IsItemDeactivatedAfterEdit — captures after-state, pushes command.
    void EndPropertyEdit();
    // For instant widgets (Checkbox, Combo) — pass pre-captured before-snapshot.
    void RecordPropertyEdit(const std::string& name, std::string beforeSnapshot);

    bool Undo();
    bool Redo();
    void Clear();

    bool CanUndo() const { return !m_UndoStack.empty(); }
    bool CanRedo() const { return !m_RedoStack.empty(); }

    std::string CaptureSnapshot();

    void SetSelectedEntityUUID(uint64_t uuid) { m_SelectedUUID = uuid; }
    uint64_t GetSelectedEntityUUID() const { return m_SelectedUUID; }

private:
    void RestoreSnapshot(const std::string& yaml);
    void TrimStack();

    std::vector<std::unique_ptr<EditorCommand>> m_UndoStack;
    std::vector<std::unique_ptr<EditorCommand>> m_RedoStack;

    std::shared_ptr<Scene>* m_ScenePtr = nullptr;
    RestoreCallback m_RestoreCallback;
    DirtyCallback m_DirtyCallback;
    uint64_t m_SelectedUUID = 0;

    // In-progress property edit state
    bool m_PropertyEditActive = false;
    std::string m_PropertyEditName;
    std::string m_PropertyEditBeforeYAML;
    uint64_t m_PropertyEditBeforeUUID = 0;
};

} // namespace VE
