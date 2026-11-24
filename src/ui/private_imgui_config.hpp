#pragma once

// The same add-on also uses ReShade's ImGui function table. Give the private
// implementation different symbols, including the classes with table wrappers.
#define ImGui UpliftImGui
#define ImGuiContext UpliftImGuiContext
struct ImGuiContext;
inline thread_local ImGuiContext* GUpliftImGui = nullptr;
#define GImGui GUpliftImGui
#define ImVector UpliftImVector
#define ImGuiIO UpliftImGuiIO
#define ImGuiPlatformIO UpliftImGuiPlatformIO
#define ImGuiViewport UpliftImGuiViewport
#define ImGuiListClipper UpliftImGuiListClipper
#define ImGuiStorage UpliftImGuiStorage
#define ImFont UpliftImFont
#define ImFontAtlas UpliftImFontAtlas
#define ImFontBaked UpliftImFontBaked
#define ImFontConfig UpliftImFontConfig
#define ImDrawData UpliftImDrawData
#define ImDrawList UpliftImDrawList
#define ImDrawListSplitter UpliftImDrawListSplitter
#define ImDrawListSharedData UpliftImDrawListSharedData
#define ImDrawCmd UpliftImDrawCmd
#define ImDrawChannel UpliftImDrawChannel
#define ImTextureData UpliftImTextureData
#define ImTextureRef UpliftImTextureRef
