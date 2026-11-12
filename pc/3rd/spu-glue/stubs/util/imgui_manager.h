/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal ImGuiManager stub. DrawDebugStateWindow() is compiled but inert --
// ImGui namespace below is a set of no-op stand-ins.
#pragma once
#include "types.h"

namespace ImGuiManager {
inline float GetGlobalScale() { return 1.0f; }
} // namespace ImGuiManager

// Tiny ImGui stand-in so spu.cpp's DrawDebugStateWindow compiles.
// Nothing actually renders -- all calls are no-ops.
struct ImVec2 { float x = 0, y = 0; ImVec2() = default; ImVec2(float a, float b): x(a), y(b) {} };
struct ImVec4 { float x = 0, y = 0, z = 0, w = 0; ImVec4() = default; ImVec4(float a, float b, float c, float d): x(a), y(b), z(c), w(d) {} };

using ImGuiCond = int;
constexpr ImGuiCond ImGuiCond_FirstUseEver = 1;
using ImGuiTreeNodeFlags = int;
constexpr ImGuiTreeNodeFlags ImGuiTreeNodeFlags_DefaultOpen = 1 << 5;

namespace ImGui {
inline bool Begin(const char*, bool* = nullptr, int = 0) { return false; }
inline void End() {}
inline void SetNextWindowSize(const ImVec2&, ImGuiCond = 0) {}
inline void Text(const char*, ...) {}
inline void TextColored(const ImVec4&, const char*, ...) {}
inline void TextUnformatted(const char*, const char* = nullptr) {}
inline void Columns(int, const char* = nullptr, bool = true) {}
inline void NextColumn() {}
inline void Separator() {}
inline void PushStyleColor(int, const ImVec4&) {}
inline void PopStyleColor(int = 1) {}
inline bool CollapsingHeader(const char*, int = 0) { return false; }
inline bool TreeNode(const char*, ...) { return false; }
inline void TreePop() {}
inline void SameLine(float = 0, float = -1) {}
inline void ProgressBar(float, const ImVec2& = ImVec2(-1, 0), const char* = nullptr) {}
inline bool IsItemHovered() { return false; }
inline void SetTooltip(const char*, ...) {}
inline bool Button(const char*, const ImVec2& = ImVec2(0, 0)) { return false; }
inline bool Checkbox(const char*, bool*) { return false; }
inline void BulletText(const char*, ...) {}
inline void Spacing() {}
inline void NewLine() {}
inline void PushStyleVar(int, float) {}
inline void PushStyleVar(int, const ImVec2&) {}
inline void PopStyleVar(int = 1) {}
inline bool SliderInt(const char*, int*, int, int, const char* = nullptr) { return false; }
inline bool InputInt(const char*, int*, int = 1, int = 100, int = 0) { return false; }
} // namespace ImGui
