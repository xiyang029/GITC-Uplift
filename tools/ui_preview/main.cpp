#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ui/overlay.hpp"
#include "ui/panel_font.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {
using Microsoft::WRL::ComPtr;
using namespace uplift::ui;

uint32_t pressed_key = 0;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(lparam & (1LL << 30))) {
    pressed_key = static_cast<uint32_t>(wparam);
  }
  if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
  switch (message) {
    case WM_DPICHANGED: {
      const auto& rect = *reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto& info = *reinterpret_cast<MINMAXINFO*>(lparam);
      info.ptMinTrackSize = {420, 350};
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

bool RunPreview(HWND window) {
  DXGI_SWAP_CHAIN_DESC swap_desc{};
  swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_desc.SampleDesc.Count = 1;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_desc.BufferCount = 2;
  swap_desc.OutputWindow = window;
  swap_desc.Windowed = TRUE;
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain> swap_chain;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &swap_desc, &swap_chain, &device, nullptr, &context))) {
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  const bool has_chinese_font = LoadPanelTypeface();

  const bool win32_ready = ImGui_ImplWin32_Init(window);
  const bool dx11_ready = win32_ready && ImGui_ImplDX11_Init(device.Get(), context.Get());
  bool ok = dx11_ready;
  if (ok) {
    Settings settings;
    settings.language = has_chinese_font ? Language::SIMPLIFIED_CHINESE : Language::ENGLISH;
    settings.pass_count = 2u;
    SetSourcePick(&settings, SourcePick::PRESENT);
    OverlayState state;
    OpenPanel(&state);
    state.panel_tab = 1;
    state.panel_focus = "ResolutionMode";
    OverlayView view;
    view.status_line = "Idle";
    view.output_size = {1620, 900};
    SetupFacts facts;
    facts.dlss_seen = true;
    facts.game_passes_motion = false;
    facts.match_game_readable = false;
    facts.split_capable = true;
    facts.frame = view.output_size;
    facts.canvas = {1262, 796};

    UINT width = 0, height = 0;
    ComPtr<ID3D11RenderTargetView> target;
    ShowWindow(window, SW_SHOWNORMAL);
    while (ok) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) && message.message != WM_QUIT) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (message.message == WM_QUIT) break;
      if (IsIconic(window)) {
        WaitMessage();
        continue;
      }
      RECT client{};
      GetClientRect(window, &client);
      const auto new_width = static_cast<UINT>(client.right);
      const auto new_height = static_cast<UINT>(client.bottom);
      if (!new_width || !new_height) {
        WaitMessage();
        continue;
      }
      if (width != new_width || height != new_height) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        ComPtr<ID3D11Texture2D> buffer;
        ok = SUCCEEDED(swap_chain->ResizeBuffers(0, new_width, new_height, DXGI_FORMAT_UNKNOWN, 0)) &&
             SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer))) &&
             SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &target));
        if (!ok) break;
        width = new_width;
        height = new_height;
      }

      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();
      const auto now = std::chrono::steady_clock::now();
      ExpireKeyCapture(&state, now);
      view.last_key_pressed = pressed_key;
      if (pressed_key && pressed_key == settings.panel_key && !state.capturing_key && !state.capturing_panel_key) {
        // Same two paths as the add-on: closing resets the intro, opening replays it from the top.
        if (state.panel_open) {
          ClosePanel(&state);
        } else {
          OpenPanel(&state);
        }
      }
      pressed_key = 0;
      facts.preferred_stage = SourcePickOf(settings);
      facts.preferred_motion = MotionPickOf(settings);
      facts.preferred_resolution = settings.resolution_mode;
      facts.pass_count = settings.pass_count;
      for (size_t i = 0; i < facts.stored_stages.size(); ++i) facts.stored_stages[i] = settings.passes[i].stage;
      FinishSetup(&view, &state, nullptr, facts, now);
      if (state.panel_open) {
        DrawOverlayWindow(view, &settings, &state, window);
      } else {
        ImGui::SetNextWindowPos({24.f, 24.f}, ImGuiCond_Once);
        if (ImGui::Begin("GITC Uplift UI Preview", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
          if (ImGui::Button("Open panel")) OpenPanel(&state);
        }
        ImGui::End();
      }
      ImGui::Render();
      constexpr float background[] = {0.11f, 0.16f, 0.22f, 1.f};
      context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
      context->ClearRenderTargetView(target.Get(), background);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
      const HRESULT present = swap_chain->Present(1, 0);
      ok = SUCCEEDED(present);
      if (present == DXGI_STATUS_OCCLUDED) Sleep(50);
    }
  }
  if (dx11_ready) ImGui_ImplDX11_Shutdown();
  if (win32_ready) ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  return ok;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  ImGui_ImplWin32_EnableDpiAwareness();
  constexpr wchar_t class_name[] = L"GITCUpliftPreview";
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = class_name;
  if (!RegisterClassW(&window_class)) return 1;

  constexpr DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rect{0, 0, 1300, 800};
  AdjustWindowRect(&rect, style, FALSE);
  HWND window = CreateWindowExW(0, class_name, L"GITC Uplift UI Preview", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
  const bool ok = window && RunPreview(window);
  if (IsWindow(window)) DestroyWindow(window);
  if (!ok) MessageBoxW(nullptr, L"Could not start or render the Direct3D 11 preview.", L"GITC Uplift UI Preview", MB_OK | MB_ICONERROR);
  return ok ? 0 : 1;
}
