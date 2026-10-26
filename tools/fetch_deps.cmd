@echo off
setlocal EnableDelayedExpansion
set "DEPS=%~dp0..\third_party\_deps"
set "RESHADE_TAG=v6.8.0"
if not exist "%DEPS%" mkdir "%DEPS%"

rem NVIDIA NGX SDK headers (never committed) at a pinned commit: the DLSS 310.9.1 SDK that Uplift is built and tested against.
set "DLSS_COMMIT=374959484e79a640feaba44c93ac8cfb0a03f5b5"
set "FETCHED_DLSS="
if exist "%DEPS%\dlss\include\nvsdk_ngx.h" for /f %%c in ('git -C "%DEPS%\dlss" rev-parse HEAD 2^>nul') do set "FETCHED_DLSS=%%c"
if "!FETCHED_DLSS!"=="%DLSS_COMMIT%" goto dlss_done
if exist "%DEPS%\dlss" rmdir /s /q "%DEPS%\dlss"
git init --quiet "%DEPS%\dlss" || exit /b 1
git -C "%DEPS%\dlss" sparse-checkout set --no-cone /include/ || exit /b 1
git -C "%DEPS%\dlss" fetch --quiet --depth 1 --filter=blob:none https://github.com/NVIDIA/DLSS.git %DLSS_COMMIT% || exit /b 1
git -C "%DEPS%\dlss" checkout --quiet FETCH_HEAD || exit /b 1
:dlss_done

rem NVIDIA Streamline SDK headers (never committed) at a pinned commit: the 2.14.16 runtime whose
rem sl:: ABI the FG multiplier hooks speak, plus the NVAPI headers that commit bundles
rem (external/nvapi), whose NV_SET_SLEEP_MODE_PARAMS the Reflex override speaks. Both are the
rem official structures; nothing here is hand-copied.
set "SL_COMMIT=625db07d4bca5abc67d781c42af7e7fb0761f1b6"
set "FETCHED_SL="
if exist "%DEPS%\streamline\include\sl_dlss_g.h" if exist "%DEPS%\streamline\external\nvapi\nvapi.h" for /f %%c in ('git -C "%DEPS%\streamline" rev-parse HEAD 2^>nul') do set "FETCHED_SL=%%c"
if "!FETCHED_SL!"=="%SL_COMMIT%" goto streamline_done
if exist "%DEPS%\streamline" rmdir /s /q "%DEPS%\streamline"
git init --quiet "%DEPS%\streamline" || exit /b 1
git -C "%DEPS%\streamline" sparse-checkout set --no-cone /include/ /external/nvapi/ || exit /b 1
git -C "%DEPS%\streamline" fetch --quiet --depth 1 --filter=blob:none https://github.com/NVIDIA-RTX/Streamline.git %SL_COMMIT% || exit /b 1
git -C "%DEPS%\streamline" checkout --quiet FETCH_HEAD || exit /b 1
:streamline_done

rem ReShade add-on API headers at the pinned tag, plus the Dear ImGui commit that tag's
rem deps/imgui submodule pins (reshade_overlay.hpp only compiles against that exact version).
set "FETCHED_TAG="
if exist "%DEPS%\reshade-tag.txt" set /p FETCHED_TAG=<"%DEPS%\reshade-tag.txt"
if "!FETCHED_TAG!"=="%RESHADE_TAG%" if exist "%DEPS%\reshade\include\reshade.hpp" if exist "%DEPS%\imgui\imgui.h" goto reshade_done
if exist "%DEPS%\reshade" rmdir /s /q "%DEPS%\reshade"
if exist "%DEPS%\imgui" rmdir /s /q "%DEPS%\imgui"
if exist "%DEPS%\reshade-tag.txt" del "%DEPS%\reshade-tag.txt"
git clone --depth 1 --branch %RESHADE_TAG% --filter=blob:none --sparse https://github.com/crosire/reshade.git "%DEPS%\reshade" || exit /b 1
git -C "%DEPS%\reshade" sparse-checkout set include || exit /b 1
set "IMGUI_COMMIT="
for /f "tokens=3" %%c in ('git -C "%DEPS%\reshade" ls-tree HEAD deps/imgui') do set "IMGUI_COMMIT=%%c"
if not defined IMGUI_COMMIT (
  echo fetch_deps: cannot read the deps/imgui commit of ReShade %RESHADE_TAG%
  exit /b 1
)
git init --quiet "%DEPS%\imgui" || exit /b 1
git -C "%DEPS%\imgui" sparse-checkout set --no-cone /imgui.h /imconfig.h /LICENSE.txt || exit /b 1
git -C "%DEPS%\imgui" fetch --quiet --depth 1 --filter=blob:none https://github.com/ocornut/imgui.git !IMGUI_COMMIT! || exit /b 1
git -C "%DEPS%\imgui" checkout --quiet FETCH_HEAD || exit /b 1
>"%DEPS%\reshade-tag.txt" echo %RESHADE_TAG%
:reshade_done

rem MinHook (BSD-2-Clause) at the commit the pinned ReShade tag's deps/minhook submodule records.
set "MINHOOK_COMMIT="
for /f "tokens=3" %%c in ('git -C "%DEPS%\reshade" ls-tree HEAD deps/minhook') do set "MINHOOK_COMMIT=%%c"
if not defined MINHOOK_COMMIT (
  echo fetch_deps: cannot read the deps/minhook commit of ReShade %RESHADE_TAG%
  exit /b 1
)
set "FETCHED_MINHOOK="
if exist "%DEPS%\minhook-commit.txt" set /p FETCHED_MINHOOK=<"%DEPS%\minhook-commit.txt"
if "!FETCHED_MINHOOK!"=="!MINHOOK_COMMIT!" if exist "%DEPS%\minhook\include\MinHook.h" goto minhook_done
if exist "%DEPS%\minhook" rmdir /s /q "%DEPS%\minhook"
if exist "%DEPS%\minhook-commit.txt" del "%DEPS%\minhook-commit.txt"
git init --quiet "%DEPS%\minhook" || exit /b 1
git -C "%DEPS%\minhook" sparse-checkout set --no-cone /include/ /src/ /LICENSE.txt || exit /b 1
git -C "%DEPS%\minhook" fetch --quiet --depth 1 --filter=blob:none https://github.com/TsudaKageyu/minhook.git !MINHOOK_COMMIT! || exit /b 1
git -C "%DEPS%\minhook" checkout --quiet FETCH_HEAD || exit /b 1
>"%DEPS%\minhook-commit.txt" echo !MINHOOK_COMMIT!
:minhook_done

echo NGX SDK headers (%DLSS_COMMIT%): %DEPS%\dlss\include
echo Streamline SDK %SL_COMMIT% headers: %DEPS%\streamline\include
echo NVAPI headers (Streamline's external/nvapi): %DEPS%\streamline\external\nvapi
echo ReShade %RESHADE_TAG% headers: %DEPS%\reshade\include
echo Dear ImGui: %DEPS%\imgui
echo MinHook: %DEPS%\minhook
exit /b 0
