@echo off
setlocal
cd /d "%~dp0..\.."
set "IMGUI=third_party\_deps\imgui"
rem Expand the existing sparse checkout at the ImGui commit pinned by ReShade.
if not exist "%IMGUI%\backends\imgui_impl_win32.cpp" (
  git -C "%IMGUI%" sparse-checkout add /imgui.cpp /imgui_draw.cpp /imgui_tables.cpp /imgui_widgets.cpp /imgui_internal.h /imstb_rectpack.h /imstb_textedit.h /imstb_truetype.h /backends/imgui_impl_dx11.* /backends/imgui_impl_win32.* || exit /b 1
)
if not exist build\ui-preview mkdir build\ui-preview || exit /b 1
cd build\ui-preview
set "IMGUI=..\..\%IMGUI%"
set "UI=..\..\src\ui"
set "PREVIEW=..\..\tools\ui_preview"
rem Compile the UI again on every run so header-only style changes are picked up.
cl /nologo /std:c++20 /EHsc /utf-8 /MT /MP /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS /DImTextureID=ImU64 ^
  /I ..\..\src /I "%IMGUI%" /I "%IMGUI%\backends" ^
  "%PREVIEW%\main.cpp" "%UI%\overlay.cpp" "%UI%\panel_font.cpp" "%UI%\localization.cpp" "%UI%\settings.cpp" "%UI%\settings_schema.cpp" ^
  "%UI%\status_text.cpp" "%UI%\controls_coalescer.cpp" ..\..\src\sources\work_size.cpp ..\..\src\color\encoding.cpp ^
  "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
  "%IMGUI%\backends\imgui_impl_dx11.cpp" "%IMGUI%\backends\imgui_impl_win32.cpp" ^
  /Fe:uplift-ui-preview.exe /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib user32.lib gdi32.lib dwmapi.lib || exit /b 1
start "" "%CD%\uplift-ui-preview.exe"
