/*
 * EditorCommand — Command Pattern implementation for undo/redo.
 *
 * CommandHistory::Execute() wraps structural operations (create, delete, etc.)
 * CommandHistory::BeginPropertyEdit/EndPropertyEdit wraps ImGui property edits.
 * All undo/redo is snapshot-based via SceneSerializer YAML.
 */

#include "VibeEngine/Editor/EditorCommand.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

// ── Helpers ──────────────────────────────────────────────────────────

std::string CommandHistory::CaptureSnapshot() {
    if (!m_ScenePtr || !*m_ScenePtr) return {};
    SceneSerializer serializer(*m_ScenePtr);
    return serializer.SerializeToString();
}

void CommandHistory::RestoreSnapshot(const std::string& yaml) {
    if (!m_ScenePtr) return;
    *m_ScenePtr = std::make_shared<Scene>();
    SceneSerializer serializer(*m_ScenePtr);
    serializer.DeserializeFromString(yaml);
    if (m_RestoreCallback) m_RestoreCallback();
}

void CommandHistory::TrimStack() {
    while (static_cast<int>(m_UndoStack.size()) > MAX_LEVELS)
        m_UndoStack.erase(m_UndoStack.begin());
}

// ── Structural operations ────────────────────────────────────────────

void CommandHistory::Execute(const std::string& name, std::function<void()> action) {
    auto cmd = std::make_unique<EditorAction>(name, std::move(action));

    cmd->UndoSnapshot = CaptureSnapshot();
    cmd->UndoSelectedUUID = m_SelectedUUID;

    static_cast<EditorAction*>(cmd.get())->Execute();

    cmd->RedoSnapshot = CaptureSnapshot();
    cmd->RedoSelectedUUID = m_SelectedUUID;

    m_UndoStack.push_back(std::move(cmd));
    m_RedoStack.clear();
    TrimStack();
}

// ── Property edit support ────────────────────────────────────────────

void CommandHistory::BeginPropertyEdit(const std::string& name) {
    // If already editing, end the previous edit first
    if (m_PropertyEditActive)
        EndPropertyEdit();

    m_PropertyEditBeforeYAML = CaptureSnapshot();
    m_PropertyEditBeforeUUID = m_SelectedUUID;
    m_PropertyEditName = name;
    m_PropertyEditActive = true;
}

void CommandHistory::EndPropertyEdit() {
    if (!m_PropertyEditActive) return;

    auto cmd = std::make_unique<PropertyEditAction>(m_PropertyEditName);
    cmd->UndoSnapshot = std::move(m_PropertyEditBeforeYAML);
    cmd->UndoSelectedUUID = m_PropertyEditBeforeUUID;
    cmd->RedoSnapshot = CaptureSnapshot();
    cmd->RedoSelectedUUID = m_SelectedUUID;

    m_UndoStack.push_back(std::move(cmd));
    m_RedoStack.clear();
    TrimStack();

    m_PropertyEditActive = false;
    m_PropertyEditBeforeYAML.clear();
}

void CommandHistory::RecordPropertyEdit(const std::string& name, std::string beforeSnapshot) {
    auto cmd = std::make_unique<PropertyEditAction>(name);
    cmd->UndoSnapshot = std::move(beforeSnapshot);
    cmd->UndoSelectedUUID = m_SelectedUUID;
    cmd->RedoSnapshot = CaptureSnapshot();
    cmd->RedoSelectedUUID = m_SelectedUUID;

    m_UndoStack.push_back(std::move(cmd));
    m_RedoStack.clear();
    TrimStack();
}

// ── Undo / Redo ──────────────────────────────────────────────────────

bool CommandHistory::Undo() {
    if (!CanUndo()) return false;

    auto cmd = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();

    m_SelectedUUID = cmd->UndoSelectedUUID;
    RestoreSnapshot(cmd->UndoSnapshot);

    VE_ENGINE_INFO("Undo: {0}", cmd->GetName());
    m_RedoStack.push_back(std::move(cmd));
    return true;
}

bool CommandHistory::Redo() {
    if (!CanRedo()) return false;

    auto cmd = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();

    m_SelectedUUID = cmd->RedoSelectedUUID;
    RestoreSnapshot(cmd->RedoSnapshot);

    VE_ENGINE_INFO("Redo: {0}", cmd->GetName());
    m_UndoStack.push_back(std::move(cmd));
    return true;
}

void CommandHistory::Clear() {
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_PropertyEditActive = false;
    m_PropertyEditBeforeYAML.clear();
}

} // namespace VE
