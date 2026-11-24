#pragma once

// Every add-on translation unit includes ReShade through this header, so imgui.h always
// precedes reshade.hpp. reshade.hpp then defines each ImGui function as an inline call through
// the function table ReShade hands out, and reshade::register_addon asks ReShade for the table
// that matches IMGUI_VERSION_NUM. CMake sets ImTextureID=ImU64 and
// IMGUI_DISABLE_OBSOLETE_FUNCTIONS to match ReShade's own ImGui build. The private
// panel compiles its own ImGui under different symbols and does not include this file.
#include <imgui.h>
#include <reshade.hpp>
