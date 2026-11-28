#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "addon/hud_present_d3d12.hpp"
#include "addon/device_context.hpp"
#include "addon/sl_tag_layout.hpp"
#include "testing/identity_host.hpp"

using Microsoft::WRL::ComPtr;
using namespace uplift;

namespace {
void Check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void Hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D12 failure: " + std::to_string(result)); }

void Policy() {
  addon::TakeResult taken;
  taken.frame = addon::HudFrame{};
  auto& h = taken.frame->hudless;
  h = {.native = 1u, .state = 0u, .lifecycle = addon::sl_layout::VALID_UNTIL_PRESENT, .described = true,
       .width = 1920u, .height = 1080u, .format = 28u, .mips = 1u, .layers = 1u, .samples = 1u};
  taken.frame->opacity = h;
  taken.frame->opacity.native = 2u;
  const addon::HudFrameFacts facts = {.width = 1920u, .height = 1080u, .format = 28u, .api = addon::TagApi::D3D12, .early = true};
  Check(addon::DecideHudFrame(true, taken, facts) == addon::HudReason::USABLE, "valid Present tags need FG evidence");
  Check(addon::DecideHudFrame(false, taken, facts) == addon::HudReason::CORRECTION_OFF, "Off used the HUD-less image");
  auto unsafe = facts;
  unsafe.effects_on = true;
  Check(addon::DecideHudFrame(true, taken, unsafe) == addon::HudReason::EFFECTS_ON, "effects after NR admitted");
  unsafe = facts;
  unsafe.technique_expected = true;
  Check(addon::DecideHudFrame(true, taken, unsafe) == addon::HudReason::EFFECTS_ON, "technique after NR admitted");
  unsafe.effects_on = true;
  unsafe.frame_generation_active = false;
  Check(addon::DecideHudFrame(true, taken, unsafe) == addon::HudReason::USABLE, "FG-off effects and technique rejected HUD-less inputs");
  unsafe.technique_expected = false;
  Check(addon::DecideHudFrame(true, taken, unsafe) == addon::HudReason::USABLE, "FG-off effects rejected HUD-less inputs");
  taken.frame->opacity.native = 0u;
  Check(addon::DecideHudFrame(true, taken, facts) == addon::HudReason::NO_OPACITY, "missing opacity was accepted");
  taken.frame->opacity = h;
  taken.frame->hudless.lifecycle = addon::sl_layout::ONLY_VALID_NOW;
  Check(addon::DecideHudFrame(true, taken, facts) == addon::HudReason::LIFECYCLE, "invalid HUD-less lifecycle admitted");
  taken.frame->hudless.lifecycle = addon::sl_layout::VALID_UNTIL_PRESENT;
  taken.frame->opacity = h;
  taken.frame->opacity.lifecycle = addon::sl_layout::ONLY_VALID_NOW;
  Check(addon::DecideHudFrame(true, taken, facts) == addon::HudReason::LIFECYCLE, "expired opacity admitted");
  taken.frame.reset();
  Check(addon::DecideHudFrame(true, taken, facts) == addon::HudReason::NOT_TAGGED, "absent tags admitted");
  std::cout << "PASS Present policy without FG, missing/expired inputs and effect timing\n";
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  const D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Transition = {.pResource = image, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = before, .StateAfter = after}};
  list->ResourceBarrier(1u, &barrier);
}

void PresentTrigger(ID3D12Device* device, ID3D12CommandQueue* queue) {
  std::string error;
  auto context = addon::DeviceContext::Create(device, {}, &error, {.host = std::make_unique<testing::IdentityHost>()});
  Check(context != nullptr, error.c_str());
  addon::FrameSettings settings;
  settings.source = ui::PlacementSource::PRESENT;
  addon::FrameFacts facts;
  facts.effects_on = true;
  const addon::TargetInfo target = {.size = {640u, 360u}, .format = DXGI_FORMAT_R16G16B16A16_FLOAT};
  const auto begin = [&](bool marker) {
    return context->BeginFrame(queue, settings, facts, target, marker, std::chrono::steady_clock::now());
  };
  // First prime the normal route's begin-effects wait, just as an already-running AKLUT preset does.
  Check(begin(false) == addon::TriggerPoint::PRESENT, "initial Present trigger changed");
  static_cast<void>(context->OnBeginEffects(true));
  context->EndFrame();
  Check(begin(false) == addon::TriggerPoint::NONE, "normal effects no longer defer NR");
  Check(context->OnBeginEffects(true) == addon::TriggerPoint::PRESENT, "normal begin-effects trigger lost");
  context->EndFrame();
  facts.hudless_present = true;
  for (const bool marker : {false, true}) {
    Check(begin(marker) == addon::TriggerPoint::PRESENT, "HUD-less NR deferred beyond the composer's Present lifetime");
    Check(context->OnBeginEffects(true) == addon::TriggerPoint::NONE, "HUD-less NR ran twice at begin-effects");
    Check(context->OnTechnique(true) == addon::TriggerPoint::NONE, "HUD-less NR ran twice at the technique");
    context->EndFrame();
  }
  facts.hudless_present = false;
  Check(begin(true) == addon::TriggerPoint::NONE, "leaving HUD-less mode lost the technique placement");
  Check(context->OnTechnique(true) == addon::TriggerPoint::MARKER, "normal technique did not resume");
  context->EndFrame();
  context->Teardown();
  std::cout << "PASS HUD-less Present timing with active effects, marker and return to ordinary placement\n";
}

void Composer() {
  ComPtr<ID3D12Device> device;
  Hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  Hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  PresentTrigger(device.Get(), queue.Get());
  std::array<ComPtr<ID3D12CommandAllocator>, 2> allocators;
  std::array<ComPtr<ID3D12GraphicsCommandList>, 2> lists;
  for (size_t i = 0; i < lists.size(); ++i) {
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    Hr(device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
  }
  ComPtr<ID3D12Fence> fence;
  Hr(device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  uint64_t progress = 0u;
  const auto wait = [&] {
    Hr(queue->Signal(fence.Get(), ++progress));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "no fence event");
    const HRESULT armed = fence->SetEventOnCompletion(progress, event);
    const DWORD waited = SUCCEEDED(armed) ? WaitForSingleObject(event, 10000u) : WAIT_FAILED;
    CloseHandle(event);
    Hr(armed);
    Check(waited == WAIT_OBJECT_0, "GPU fence timed out");
  };
  const auto execute = [&](size_t index) {
    Hr(lists[index]->Close());
    ID3D12CommandList* commands[] = {lists[index].Get()};
    queue->ExecuteCommandLists(1u, commands);
  };
  const auto buffer = [&](D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state) {
    const D3D12_HEAP_PROPERTIES heap = {.Type = heap_type};
    const D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 1536u, .Height = 1u, .DepthOrArraySize = 1u,
        .MipLevels = 1u, .SampleDesc = {1u, 0u}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> resource;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
  };
  auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t* bytes = nullptr;
  Hr(upload->Map(0u, nullptr, reinterpret_cast<void**>(&bytes)));
  // F = H = 0.2 except for one HUD pixel; N after the simulated NR pass = 0.6.
  for (size_t row = 0; row < 3u; ++row) {
    for (size_t pixel = 0; pixel < 3u; ++pixel) {
      for (size_t channel = 0; channel < 4u; ++channel) {
        bytes[row * 512u + pixel * 4u + channel] = (row == 2u ? static_cast<uint8_t>(pixel == 0u ? 0u : pixel == 1u ? 128u : 255u)
            : row == 0u && pixel == 2u && channel < 3u ? 204u : (channel == 3u ? 255u : 51u));
      }
    }
  }
  upload->Unmap(0u, nullptr);
  std::array<ComPtr<ID3D12Resource>, 3> images;
  for (size_t i = 0; i < images.size(); ++i) {
    const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = 3u, .Height = 1u, .DepthOrArraySize = 1u,
        .MipLevels = 1u, .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = {1u, 0u}, .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&images[i])));
    const D3D12_TEXTURE_COPY_LOCATION to = {.pResource = images[i].Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    const D3D12_TEXTURE_COPY_LOCATION from = {.pResource = upload.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {.Offset = i * 512u, .Footprint = {.Format = desc.Format, .Width = 3u, .Height = 1u, .Depth = 1u, .RowPitch = 256u}}};
    lists[0]->CopyTextureRegion(&to, 0u, 0u, 0u, &from, nullptr);
    Barrier(lists[0].Get(), images[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, i == 0u ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  execute(0u);
  wait();
  Hr(allocators[0]->Reset());
  Hr(lists[0]->Reset(allocators[0].Get(), nullptr));
  color::HudComposerD3D12 composer;
  std::string error;
  Check(composer.Initialize(device.Get(), &error), error.c_str());
  addon::HudTags tags;
  tags.SetApi(addon::TagApi::D3D12);
  tags.SetStoring(true);
  addon::FgInput input;
  Check(!input.FrameGenerationReads(std::chrono::steady_clock::now()), "test unexpectedly has FG reads");
  addon::HudSettle settle;
  const addon::HudPresentD3D12 present = {.composer = &composer, .tags = &tags, .input = &input, .settle = &settle, .use_wanted = true, .early = true,
      .effects_on = true, .technique_expected = true, .frame_generation_active = false};
  std::array<addon::sl_layout::Resource, 2> tagged;
  std::array<addon::sl_layout::ResourceTag, 2> resource_tags;
  for (size_t i = 0; i < tagged.size(); ++i) {
    tagged[i].native = images[i + 1u].Get();
    tagged[i].state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    resource_tags[i] = {.base = {.type = addon::sl_layout::RESOURCE_TAG_TYPE}, .resource = &tagged[i],
        .type = i == 0u ? addon::sl_layout::HUDLESS_COLOR : addon::sl_layout::UI_ALPHA, .lifecycle = addon::sl_layout::VALID_UNTIL_PRESENT};
  }
  addon::HudBegun begun;
  tags.Record(nullptr, resource_tags.data(), static_cast<uint32_t>(resource_tags.size()));
  const auto counted = tags.Totals();
  Check(counted.last_hudless_ms != 0u && counted.last_opacity_ms != 0u, "non-empty tags were not marked recent");
  tags.Clear();
  Check(tags.Totals().last_hudless_ms == 0u && tags.Totals().last_opacity_ms == 0u
        && tags.Totals().hudless == counted.hudless, "clearing tags retained freshness or lost lifetime counts");
  for (uint32_t frame = 1u; frame <= addon::HUD_SETTLE_PRESENTS; ++frame) {
    tags.Record(nullptr, resource_tags.data(), static_cast<uint32_t>(resource_tags.size()));
    begun = addon::BeginHudD3D12(present, lists[0].Get(), images[0].Get(), frame);
    Check(frame == addon::HUD_SETTLE_PRESENTS ? begun.n != nullptr : begun.reason == addon::HudReason::SETTLING, "FG-off tags did not settle");
    const auto consumed = tags.Totals();
    Check(consumed.last_hudless_ms != 0u && consumed.last_opacity_ms != 0u, "consuming valid HUD inputs erased their arrival timestamps");
  }
  execute(0u);  // The Present copy is flushed before NR, which records on another native list.
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1u};
  Hr(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(begun.n, nullptr, rtv);
  Barrier(lists[1].Get(), begun.n, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const float corrected[] = {0.6f, 0.6f, 0.6f, 1.f};
  lists[1]->ClearRenderTargetView(rtv, corrected, 0u, nullptr);
  Barrier(lists[1].Get(), begun.n, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  composer.End(lists[1].Get());
  composer.Close(lists[1].Get());
  Barrier(lists[1].Get(), images[0].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_TEXTURE_COPY_LOCATION destination = {.pResource = readback.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = {.Footprint = {.Format = DXGI_FORMAT_R8G8B8A8_UNORM, .Width = 3u, .Height = 1u, .Depth = 1u, .RowPitch = 256u}}};
  const D3D12_TEXTURE_COPY_LOCATION source = {.pResource = images[0].Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  lists[1]->CopyTextureRegion(&destination, 0u, 0u, 0u, &source, nullptr);
  Barrier(lists[1].Get(), images[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  execute(1u);
  composer.Submitted(queue.Get());
  wait();
  Hr(readback->Map(0u, nullptr, reinterpret_cast<void**>(&bytes)));
  const std::array<int, 3> expected = {153, 102, 204};
  bool matches = true;
  for (size_t pixel = 0; pixel < expected.size(); ++pixel) {
    for (size_t channel = 0; channel < 3u; ++channel) matches = matches && std::abs(static_cast<int>(bytes[pixel * 4u + channel]) - expected[pixel]) <= 1;
  }
  readback->Unmap(0u, nullptr);
  Check(matches, "HUD opacity composition changed clear, partial or opaque pixels");

  Hr(allocators[0]->Reset());
  Hr(lists[0]->Reset(allocators[0].Get(), nullptr));
  tags.Clear();
  addon::sl_layout::ResourceTag hudless_tag = {.base = {.type = addon::sl_layout::RESOURCE_TAG_TYPE}, .resource = &tagged[0],
      .type = addon::sl_layout::HUDLESS_COLOR, .lifecycle = addon::sl_layout::VALID_UNTIL_PRESENT};
  tags.Record(nullptr, &hudless_tag, 1u);
  Check(tags.Totals().last_hudless_ms != 0u && tags.Totals().last_opacity_ms == 0u, "HUD-less-only tag accounting retained opacity freshness");
  begun = addon::BeginHudD3D12(present, lists[0].Get(), images[0].Get(), addon::HUD_SETTLE_PRESENTS + 1u);
  Check(begun.n == nullptr && begun.reason == addon::HudReason::NO_OPACITY, "D3D12 composer accepted missing opacity");
  Hr(lists[0]->Close());
  tags.Record(nullptr, resource_tags.data(), static_cast<uint32_t>(resource_tags.size()));
  tags.SetStoring(false);
  Check(!tags.Take().frame, "turning correction off retained tags");
  std::cout << "PASS D3D12 FG-off tags, settle, command-list rotation, opacity composition and missing-opacity rejection\n";

  // The overlay reads Status after BeginFrame, following the native-NR evaluate on the game list.
  for (const auto placement_source : {ui::PlacementSource::AUTO, ui::PlacementSource::DLSS, ui::PlacementSource::PRESENT}) {
  for (const bool replacing : {true, false}) {
    auto context = addon::DeviceContext::Create(device.Get(), {}, &error, {.host = std::make_unique<testing::IdentityHost>()});
    Check(context != nullptr, error.c_str());
    addon::FrameSettings settings;
    settings.source = ui::PlacementSource::PRESENT;
    settings.session.enabled = true;
    settings.encoding = color::Encoding::LINEAR_BT709;
    settings.native_nr_mode = replacing ? ui::NativeNrMode::REPLACE : ui::NativeNrMode::AFTER;
    settings.resolution = ui::ResolutionMode::CUSTOM;
    settings.resolution_scale = 50;
    settings.resolution_asymmetric = true;
    settings.resolution_scale_y = 60;
    settings.foveation = true;
    settings.foveation_width = settings.foveation_height = 55;
    settings.foveation_falloff_x = 28;
    settings.foveation_falloff_y = 15;
    settings.foveation_edge_x = 25;
    settings.foveation_edge_y = 50;
    settings.stage_resolution[addon::LATER_AFTER_DLSS_SLOT].mode = ui::ResolutionMode::FULL;
    const addon::TargetInfo target = {.size = {2560u, 1440u}, .format = DXGI_FORMAT_R16G16B16A16_FLOAT};
    const auto begin = [&] { context->BeginFrame(queue.Get(), settings, {}, target, false, std::chrono::steady_clock::now()); };
    for (unsigned attempt = 0u; attempt < 1000u; ++attempt) {
      begin();
      if (context->Status().session.state == nr::SessionState::ACTIVE) break;
      Sleep(1u);
    }
    Check(context->Status().session.state == nr::SessionState::ACTIVE, "native NR test runtime did not load");
    const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = target.size.width, .Height = target.size.height,
        .DepthOrArraySize = 1u, .MipLevels = 1u, .Format = target.format, .SampleDesc = {1u, 0u},
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> image;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&image)));
    Hr(allocators[0]->Reset());
    Hr(lists[0]->Reset(allocators[0].Get(), nullptr));
    struct Host final : sources::DlssFrameHost {
      ID3D12GraphicsCommandList* list = nullptr;
      bool known = true;
      std::vector<uint64_t> tokens;
      std::vector<std::shared_ptr<sources::NrTimingSlot>> timings;
      ID3D12GraphicsCommandList* NativeList() override { return list; }
      bool StateKnown() override { return known; }
      void ReplayState(state::RestoreMode) override {}
      void AttachToken(uint64_t token) override { tokens.push_back(token); }
      void HoldTiming(const std::shared_ptr<sources::NrTimingSlot>& slot) override { timings.push_back(slot); }
    } host;
    host.list = lists[0].Get();
    const ngx_hooks::DlssFrame frame = {.color = image.Get(), .output = image.Get(),
        .color_region = {0u, 0u, target.size.width, target.size.height}, .output_region = {0u, 0u, target.size.width, target.size.height}};
    static_cast<void>(context->OnDlssEvaluate(frame, host, std::chrono::steady_clock::now()));
    settings.source = placement_source;
    begin();
    const auto recorded = context->OnNativeNrEvaluate(frame, host, std::chrono::steady_clock::now(),
        replacing ? addon::NativeNrPath::REPLACE : addon::NativeNrPath::AFTER);
    Check(recorded.nr_applied && recorded.passes_run == 1u, "native NR test did not record a frame");
    execute(0u);
    context->SubmitTokens(host.tokens, queue.Get(), GetCurrentThreadId(), std::chrono::steady_clock::now());
    wait();
    context->StampThread(GetCurrentThreadId());
    begin();
    const auto status = context->Status();
    const auto card = ui::BuildStatusCard({.enabled = true, .session = status.session, .nr_applied = status.nr_applied,
        .passes_run = status.passes_run, .skip_reason = status.skip_reason});
    Check(status.nr_applied && card.working, "Present cleared a successful native NR recording before the overlay read it");
    Check(status.group_count == 1u && status.groups[0].stage == addon::Stage::AFTER_DLSS && status.groups[0].nr_applied
          && status.groups[0].passes_run == 1u && status.groups[0].work == nr::Size{998u, 734u},
          "native NR details read the stored Source instead of the actual execution stage");
    Check(status.session.group_count == 1u && status.session.groups[0].stage == static_cast<uint8_t>(addon::Stage::AFTER_DLSS),
          "native NR runtime and timing stage differ from the reported execution stage");
    begin();
    // Ordinary DLSS placements retain their last evaluate; Present reports each frame afresh.
    if (placement_source == ui::PlacementSource::PRESENT) {
      Check(!context->Status().nr_applied, "native NR stayed live after a Present frame with no evaluate");
    }
    host.known = false;
    const auto skipped = context->OnNativeNrEvaluate(frame, host, std::chrono::steady_clock::now(),
        replacing ? addon::NativeNrPath::REPLACE : addon::NativeNrPath::AFTER);
    Check(!skipped.nr_applied && skipped.reason == nr::SkipReason::LIST_STATE_UNKNOWN, "native NR skip was not exercised");
    begin();
    Check(!context->Status().nr_applied && context->Status().skip_reason == nr::SkipReason::LIST_STATE_UNKNOWN,
          "Present discarded the native NR skip reason");
    settings.native_nr_mode = ui::NativeNrMode::OFF;
    settings.source = ui::PlacementSource::PRESENT;
    begin();
    Check(context->Status().groups[0].stage == addon::Stage::PRESENT, "normal execution stage was not restored");
    context->Teardown();
  }
  }
  std::cout << "PASS native NR replacement/follow-up resolution across Sources, activity, details, skips and mode changes\n";
}
}  // namespace

int main() {
  try { Policy(); Composer(); return 0; }
  catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
