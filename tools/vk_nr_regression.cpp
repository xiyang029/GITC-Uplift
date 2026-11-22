#include <Windows.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "color/color_pipeline.hpp"
#include "color/vk_color_pipeline.hpp"
#include "nr/vk_handles.hpp"
#include "sources/vk_nr_pipeline.hpp"
#include "vk/interop.hpp"

using namespace uplift;
void Check(bool yes, const char* what) { if (!yes) throw std::runtime_error(what); }
void Vk(VkResult r) { if (r != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(r)); }
constexpr VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
std::atomic<unsigned> errors{0u};
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                    const VkDebugUtilsMessengerCallbackDataEXT* message, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { ++errors; std::cerr << message->pMessage << '\n'; }
  return VK_FALSE;
}

struct Gpu {
  HMODULE module = LoadLibraryW(L"vulkan-1.dll");
  PFN_vkGetInstanceProcAddr gi = nullptr; PFN_vkGetDeviceProcAddr gd = nullptr;
  VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice physical = VK_NULL_HANDLE; VkDevice device = VK_NULL_HANDLE;
  std::array<VkQueue, 2> queues{}; uint32_t family = 0u, queue_count = 0u;
  VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer commands = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE; bool validation = false;
  vk::NrFunctions f; VkPhysicalDeviceMemoryProperties memory{}; color::VkColorPipeline color;
  PFN_vkCmdCopyBufferToImage upload = nullptr; PFN_vkDeviceWaitIdle idle = nullptr;
  struct Buffer { VkBuffer buffer; VkDeviceMemory memory; void* mapped; };
  std::vector<Buffer> buffers; std::vector<vk::NrImage> images;
  template<class T> T I(const char* name) { return reinterpret_cast<T>(gi(instance, name)); }
  template<class T> T D(const char* name) { return reinterpret_cast<T>(gd(device, name)); }
  Gpu() {
    Check(module != nullptr, "Vulkan loader unavailable"); gi = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
    auto enumerate_layers = I<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties");
    uint32_t count = 0u; Vk(enumerate_layers(&count, nullptr)); std::vector<VkLayerProperties> available(count); Vk(enumerate_layers(&count, available.data()));
    for (auto& l : available) validation |= std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features{.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .enabledValidationFeatureCount = 1u, .pEnabledValidationFeatures = &sync};
    VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Uplift NR regression", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo create{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = validation ? &features : nullptr, .pApplicationInfo = &app,
      .enabledLayerCount = validation ? 1u : 0u, .ppEnabledLayerNames = &layer, .enabledExtensionCount = validation ? 2u : 1u, .ppEnabledExtensionNames = extensions};
    Vk(I<PFN_vkCreateInstance>("vkCreateInstance")(&create, nullptr, &instance));
    VkDebugUtilsMessengerCreateInfoEXT debug{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = Debug};
    Vk(I<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT")(instance, &debug, nullptr, &messenger));
    auto enumerate = I<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices"); Vk(enumerate(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count); Vk(enumerate(instance, &count, devices.data()));
    for (auto d : devices) { VkPhysicalDeviceProperties p{}; I<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(d, &p);
      if (p.vendorID == 0x10de) { physical = d; std::cout << "Vulkan GPU: " << p.deviceName << ", sync validation=" << (validation ? "on" : "unavailable") << '\n'; break; } }
    Check(physical != VK_NULL_HANDLE, "NVIDIA Vulkan GPU unavailable");
    auto families = I<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties"); families(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count); families(physical, &count, props.data());
    for (uint32_t i = 0; i < count; ++i) if ((props[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { family = i; queue_count = std::min(2u, props[i].queueCount); break; }
    Check(queue_count != 0u, "no graphics/compute queue");
    const float priorities[] = {1.f, 1.f}; VkDeviceQueueCreateInfo q{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = queue_count, .pQueuePriorities = priorities};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE};
    VkPhysicalDeviceFeatures supported{}; I<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical, &supported);
    VkPhysicalDeviceFeatures enabled{}; enabled.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
    const char* push = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    VkDeviceCreateInfo dc{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &timeline, .queueCreateInfoCount = 1u, .pQueueCreateInfos = &q, .enabledExtensionCount = 1u, .ppEnabledExtensionNames = &push, .pEnabledFeatures = &enabled};
    Vk(I<PFN_vkCreateDevice>("vkCreateDevice")(physical, &dc, nullptr, &device)); gd = I<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
#define RESOLVE(name) f.name = D<PFN_##name>(#name); Check(f.name != nullptr, #name);
    UPLIFT_VK_NR_FUNCTIONS(RESOLVE) UPLIFT_VK_NR_FUNCTIONS_WITH_KHR_ALIAS(RESOLVE) RESOLVE(vkCmdPushDescriptorSetKHR)
#undef RESOLVE
    upload = D<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage"); idle = D<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    for (uint32_t i = 0; i < queue_count; ++i) D<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, i, &queues[i]);
    I<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
    VkCommandPoolCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family}; Vk(f.vkCreateCommandPool(device, &pc, nullptr, &pool));
    VkCommandBufferAllocateInfo ca{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u}; Vk(f.vkAllocateCommandBuffers(device, &ca, &commands));
    VkFenceCreateInfo fc{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; Vk(f.vkCreateFence(device, &fc, nullptr, &fence));
    std::string error; Check(color.Initialize(f, device, memory, &error), error.c_str()); Begin();
  }
  void Begin() { VkCommandBufferBeginInfo b{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Vk(f.vkBeginCommandBuffer(commands, &b)); }
  void Submit() {
    Vk(f.vkEndCommandBuffer(commands)); VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &commands};
    Vk(f.vkQueueSubmit(queues[0], 1u, &submit, fence)); Vk(f.vkWaitForFences(device, 1u, &fence, VK_TRUE, 10000000000ull));
    Vk(f.vkResetFences(device, 1u, &fence)); Vk(f.vkResetCommandPool(device, pool, 0u)); Begin();
  }
  Buffer MakeBuffer(size_t bytes) {
    Buffer out{}; VkBufferCreateInfo b{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    Vk(f.vkCreateBuffer(device, &b, nullptr, &out.buffer)); VkMemoryRequirements r{}; f.vkGetBufferMemoryRequirements(device, out.buffer, &r);
    const uint32_t type = vk::FindMemoryType(memory, r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(type != UINT32_MAX, "host coherent memory unavailable"); VkMemoryAllocateInfo a{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = type};
    Vk(f.vkAllocateMemory(device, &a, nullptr, &out.memory)); Vk(f.vkBindBufferMemory(device, out.buffer, out.memory, 0u)); Vk(f.vkMapMemory(device, out.memory, 0u, VK_WHOLE_SIZE, 0u, &out.mapped));
    buffers.push_back(out); return out;
  }
  void Transition(VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = range};
    f.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
  }
  vk::NrImage Image(uint32_t w, uint32_t h, VkFormat format, const void* data = nullptr, uint32_t pixel_bytes = 0u) {
    vk::NrImage image; Check(vk::CreateNrImage(f, device, memory, format, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, &image), "Vulkan image allocation failed");
    images.push_back(image); Transition(image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    if (data) { auto b = MakeBuffer(size_t(w) * h * pixel_bytes); std::memcpy(b.mapped, data, size_t(w) * h * pixel_bytes);
      VkBufferImageCopy c{.imageSubresource = layers, .imageExtent = {w, h, 1u}}; upload(commands, b.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &c); Transition(image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL); }
    return image;
  }
  Buffer Read(const vk::NrImage& image, uint32_t w, uint32_t h, uint32_t pixel_bytes) {
    auto b = MakeBuffer(size_t(w) * h * pixel_bytes); Transition(image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{.imageSubresource = layers, .imageExtent = {w, h, 1u}}; f.vkCmdCopyImageToBuffer(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1u, &c);
    VkBufferMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = b.buffer, .size = VK_WHOLE_SIZE};
    f.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, nullptr, 1u, &barrier, 0u, nullptr); return b;
  }
  void Clear() { for (auto& i : images) vk::DestroyNrImage(f, device, &i); images.clear();
    for (auto& b : buffers) { f.vkUnmapMemory(device, b.memory); f.vkDestroyBuffer(device, b.buffer, nullptr); f.vkFreeMemory(device, b.memory, nullptr); } buffers.clear(); }
  ~Gpu() { if (!device) return; idle(device); Clear(); color.Destroy(); f.vkDestroyFence(device, fence, nullptr); f.vkDestroyCommandPool(device, pool, nullptr); D<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
    I<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT")(instance, messenger, nullptr); I<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr); FreeLibrary(module); }
};

void Fusion(Gpu& gpu) {
  constexpr uint32_t w = 79, h = 61, iw = 67, ih = 53; unsigned cases = 0;
  for (auto encoding : {color::Encoding::SRGB, color::Encoding::PQ_BT2100, color::Encoding::SCRGB, color::Encoding::LINEAR_BT709})
    for (bool preserved : {false, true}) for (bool masked : {false, true}) for (bool subrect : {false, true}) {
      std::vector<float> source(w*h*4), mask(w*h*4); std::vector<uint16_t> output(iw*ih*4);
      for (uint32_t i=0; i<w*h; ++i) { for (uint32_t c=0; c<4; ++c) source[4*i+c] = float((i*97u+c*31u)%1024)/1023.f; if (encoding == color::Encoding::SCRGB) source[4*i] = source[4*i]*12.f-.2f; mask[4*i] = i%3==0 ? 0.f : i%3==1 ? .35f : 1.f; }
      for (uint32_t i=0; i<output.size(); ++i) output[i]=DirectX::PackedVector::XMConvertFloatToHalf(float((i*37u)%911)/1024.f);
      auto src = gpu.Image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,source.data(),16), m = gpu.Image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,mask.data(),16);
      auto out = gpu.Image(iw,ih,VK_FORMAT_R16G16B16A16_SFLOAT,output.data(),8), input=gpu.Image(iw,ih,VK_FORMAT_R16G16B16A16_SFLOAT), change=gpu.Image(iw,ih,VK_FORMAT_R16G16B16A16_SFLOAT);
      auto old = gpu.Image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,source.data(),16), fused = gpu.Image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,source.data(),16);
      gpu.color.PrepareRecording(gpu.commands);
      nr::Rect region{.x=subrect?5u:0u,.y=subrect?3u:0u,.width=iw,.height=ih};
      color::VkEncodePass encode{.source=src.view,.source_layout=VK_IMAGE_LAYOUT_GENERAL,.source_format=VK_FORMAT_R32G32B32A32_SFLOAT,.model=input.view,.width=iw,.height=ih,.encoding=encoding,.input_scale=.7f,.region=region,.canvas={iw,ih},.options=color::shader_options::NEAR_BLACK_GUARD};
      Check(gpu.color.RecordEncode(gpu.commands,0u,encode),"Vulkan encode failed"); gpu.color.ComputeBarrier(gpu.commands);
      Check(gpu.color.RecordChange(gpu.commands,0u,{.encode=encode,.nr_output=out.view,.change=change.view,.model_input=preserved?input.view:VK_NULL_HANDLE}),"Vulkan change failed"); gpu.color.ComputeBarrier(gpu.commands);
      color::VkDecodePass d{.target=old.view,.target_format=VK_FORMAT_R32G32B32A32_SFLOAT,.nr_output=change.view,.origin_x=region.x,.origin_y=region.y,.target_width=w,.target_height=h,.width=iw,.height=ih,.encoding=encoding,.input_scale=.7f,.transfer_strength=.8f,.color_strength=.6f,.source_x=region.x,.source_y=region.y,.options=color::shader_options::NEAR_BLACK_GUARD|color::shader_options::CONSISTENT,.mask={.view=masked?m.view:VK_NULL_HANDLE}};
      Check(gpu.color.RecordDecode(gpu.commands,0u,d),"Vulkan reference decode failed"); d.target=fused.view;d.nr_output=out.view;d.model_input=preserved?input.view:VK_NULL_HANDLE;d.options|=color::shader_options::FUSED_CHANGE;
      Check(gpu.color.RecordDecode(gpu.commands,1u,d),"Vulkan fused decode failed");
      auto a=gpu.Read(old,w,h,16), b=gpu.Read(fused,w,h,16);gpu.Submit(); Check(std::memcmp(a.mapped,b.mapped,w*h*16)==0,"SPIR-V fusion changed pixels");gpu.Clear();++cases;
    }
  std::cout << "PASS Vulkan " << cases << " bit-exact fusion cases (SDR/HDR10/scRGB/linear, alpha/mask/subrect)\n";
}

class IdentityHost final : public nr::Host {
 public:
  explicit IdentityHost(Gpu& gpu):gpu_(gpu){}
  class Feature final: public nr::FeatureInterface {
   public:
    Feature(Gpu& gpu, bool& reset):gpu_(gpu),reset_(reset){}
    NVSDK_NGX_Result Create(ID3D12GraphicsCommandList*,const nr::CreateInfo& info)override{size_=info.capacity;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* list,const nr::EvaluateInputs& input,const nr::Controls&)override{
      reset_=input.reset;
      auto& a=nr::VkResourceOf(input.color)->Resource.ImageViewInfo;auto& b=nr::VkResourceOf(input.output)->Resource.ImageViewInfo;
      VkImageCopy copy{.srcSubresource=layers,.dstSubresource=layers,.extent={a.Width,a.Height,1u}};gpu_.f.vkCmdCopyImage(nr::VkListOf(list),a.Image,VK_IMAGE_LAYOUT_GENERAL,b.Image,VK_IMAGE_LAYOUT_GENERAL,1u,&copy);return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Release()override{size_={};return NVSDK_NGX_Result_Success;}void Abandon()override{size_={};}
    bool Created()const override{return !size_.Empty();}nr::Size Capacity()const override{return size_;}
   private:Gpu& gpu_;bool& reset_;nr::Size size_;
  };
  NVSDK_NGX_Result LoadRuntime()override{return NVSDK_NGX_Result_Success;}void UnloadRuntime()override{}void AbandonRuntime()override{}
  std::unique_ptr<nr::FeatureInterface> NewFeature()override{return std::make_unique<Feature>(gpu_,last_reset);}
  bool last_reset=false;
  nr::MemoryInfo QueryMemory()override{return{.budget=uint64_t{16}<<30,.usage=0u};}std::optional<uint64_t> QueryRuntimeBytes()override{return 0u;}
  std::chrono::steady_clock::time_point Now()override{return std::chrono::steady_clock::now();}
  void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource*,D3D12_RESOURCE_STATES,D3D12_RESOURCE_STATES)override{
    VkMemoryBarrier b{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    gpu_.f.vkCmdPipelineBarrier(nr::VkListOf(list),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0u,1u,&b,0u,nullptr,0u,nullptr);
  }
 private:Gpu& gpu_;
};

void Present(Gpu& gpu) {
  IdentityHost host(gpu);nr::Timeline timeline([]{return UINT64_MAX;},[](auto*,auto){},[](auto,auto){return true;});nr::Session session(host,timeline);
  session.SetEnabled(true);session.NoteFrameSize({1280,720});
  for(unsigned i=0;i<100u&&session.State()!=nr::SessionState::ACTIVE;++i){session.Tick();Sleep(1u);}
  Check(session.State()==nr::SessionState::ACTIVE,"identity Vulkan session unavailable");
  {
    sources::VkNrPipeline pipeline(gpu.f,gpu.device,gpu.memory,session,timeline);std::string error;Check(pipeline.Initialize(&error),error.c_str());
    for(auto format:{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_A2B10G10R10_UNORM_PACK32,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT}){
      const uint32_t bytes=format==VK_FORMAT_R16G16B16A16_SFLOAT?8u:format==VK_FORMAT_R32G32B32A32_SFLOAT?16u:4u;
      std::vector<uint32_t> pixels(size_t(1280)*720*bytes/4u);
      for(size_t i=0;i<pixels.size();++i) {
        if(bytes==4u)pixels[i]=uint32_t(i*747796405u+2891336453u);
        else if(bytes==8u){auto x=DirectX::PackedVector::XMConvertFloatToHalf(float(i%1024)/1023.f);auto y=DirectX::PackedVector::XMConvertFloatToHalf(float((i*97)%1024)/1023.f);pixels[i]=uint32_t(x)|(uint32_t(y)<<16);}
        else{float x=float((i*97)%1024)/1023.f;std::memcpy(&pixels[i],&x,4u);}
      }
      std::vector<std::byte> reference;uint64_t held=0u;
      for(bool direct:{false,true}){
        auto image=gpu.Image(1280,720,format,pixels.data(),bytes);
        auto result=pipeline.RecordPresent(gpu.commands,{.image=image.image,.layout=VK_IMAGE_LAYOUT_GENERAL,.size={1280,720},.copy_format=format,.image_format=direct?format:VK_FORMAT_UNDEFINED},nr::Controls{},true);
        Check(result.nr_applied,"Vulkan Present did not apply identity NR");auto output=gpu.Read(image,1280,720,bytes);gpu.Submit();
        if(!direct){reference.resize(size_t(1280)*720*bytes);std::memcpy(reference.data(),output.mapped,reference.size());held=pipeline.HeldBytes();}
        else{Check(std::memcmp(reference.data(),output.mapped,reference.size())==0,"direct Vulkan Present changed bits");
          const uint64_t expected=format==VK_FORMAT_R16G16B16A16_SFLOAT?0u:uint64_t(1280)*720*bytes;Check(held-pipeline.HeldBytes()==expected,"Present staging was not eliminated");}
        gpu.Clear();
      }
    }
    std::vector<uint16_t> pixels(size_t(1280)*720*4u,DirectX::PackedVector::XMConvertFloatToHalf(.25f));
    auto image=gpu.Image(1280,720,VK_FORMAT_R16G16B16A16_SFLOAT,pixels.data(),8u);
    sources::LookConfig config;
    const auto frame=[&](color::Primaries primaries,bool reset,bool expected){
      config.fixes.primaries=primaries;pipeline.SetLookConfig(config);
      auto result=pipeline.RecordPresent(gpu.commands,{.image=image.image,.layout=VK_IMAGE_LAYOUT_GENERAL,.size={1280,720},
        .copy_format=VK_FORMAT_R16G16B16A16_SFLOAT,.encoding=color::Encoding::LINEAR_BT709},nr::Controls{},reset);
      Check(result.nr_applied && host.last_reset==expected,"Vulkan colour basis or explicit reset was lost/repeated");gpu.Submit();
    };
    frame(color::Primaries::AUTO,false,true);frame(color::Primaries::BT709,false,false);
    frame(color::Primaries::BT2020,false,true);frame(color::Primaries::BT2020,false,false);
    frame(color::Primaries::AP1,false,true);frame(color::Primaries::AP1,true,true);
    frame(color::Primaries::AP1,true,true);frame(color::Primaries::AP1,false,false);
    std::cout<<"PASS Vulkan resolved colour-basis history invalidation and consecutive explicit resets\n";
    pipeline.ReleaseNrSurfaces();timeline.Flush(std::chrono::milliseconds(0));gpu.idle(gpu.device);
  }
  session.OnDeviceDestroyed();
  std::cout<<"PASS Vulkan production Present bit-exact staging/direct paths, 8/10/16/32-bit formats and held-byte reduction\n";
}

void QueueOrder(Gpu& gpu) {
  Check(gpu.queue_count >= 2u, "two queues are required for the queue-order regression");
  struct Host final : vk::GameHost {
    Gpu& gpu; VkQueue queue; unsigned signals=0u, waits=0u; bool fail_signal=false, fail_wait=false;
    Host(Gpu& g,VkQueue q):gpu(g),queue(q){}
    bool Valid()const override{return true;}VkQueue Queue()const override{return queue;}
    void Barrier(VkImage,vk::Usage,vk::Usage)override{}
    void BarrierWithGlobal(VkImage,vk::Usage,vk::Usage,vk::Usage,vk::Usage)override{}
    void Copy(VkImage,VkImage)override{}
    bool Signal(VkSemaphore semaphore,uint64_t value)override{
      ++signals;if(fail_signal)return false;
      VkTimelineSemaphoreSubmitInfo timeline{.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,.signalSemaphoreValueCount=1u,.pSignalSemaphoreValues=&value};
      VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&timeline,.signalSemaphoreCount=1u,.pSignalSemaphores=&semaphore};return gpu.f.vkQueueSubmit(queue,1u,&submit,VK_NULL_HANDLE)==VK_SUCCESS;
    }
    bool Wait(VkSemaphore semaphore,uint64_t value)override{
      ++waits;if(fail_wait)return false;const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkTimelineSemaphoreSubmitInfo timeline{.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,.waitSemaphoreValueCount=1u,.pWaitSemaphoreValues=&value};
      VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&timeline,.waitSemaphoreCount=1u,.pWaitSemaphores=&semaphore,.pWaitDstStageMask=&stage};return gpu.f.vkQueueSubmit(queue,1u,&submit,VK_NULL_HANDLE)==VK_SUCCESS;
    }
    bool FlushWithFence(VkFence fence)override{VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO};return gpu.f.vkQueueSubmit(queue,1u,&submit,fence)==VK_SUCCESS;}
    void DestroyImage(VkImage image)override{gpu.f.vkDestroyImage(gpu.device,image,nullptr);}
  } first(gpu,gpu.queues[0]),second(gpu,gpu.queues[1]);
  vk::Device device;device.handle=gpu.device;device.record.timeline=true;
  device.vkCreateSemaphore=gpu.f.vkCreateSemaphore;device.vkDestroySemaphore=gpu.f.vkDestroySemaphore;
  device.vkCreateFence=gpu.f.vkCreateFence;device.vkDestroyFence=gpu.f.vkDestroyFence;device.vkGetFenceStatus=gpu.f.vkGetFenceStatus;
  device.vkWaitForFences=gpu.f.vkWaitForFences;device.vkResetFences=gpu.f.vkResetFences;
  device.vkGetSemaphoreCounterValue=gpu.f.vkGetSemaphoreCounterValue;
  vk::Interop interop(device);
  Check(interop.OrderQueues(first,first,100u)==vk::WaitResult::DONE&&first.signals==0u,"same queue needed an extra submission");
  for(unsigned i=0;i<128u;++i)Check(interop.OrderQueues(first,second,100u)==vk::WaitResult::DONE,"GPU dependency failed");
  Check(interop.OrderQueues(second,first,100u)==vk::WaitResult::DONE,"reverse queue dependency failed");
  Check(!interop.RetireQueueDependencies(second),"retired another consumer's semaphore using the wrong fence");
  Check(interop.RetireQueueDependencies(first),"queue-pair dependencies did not retire");
  Vk(gpu.idle(gpu.device));interop.FreeFinished(first);Check(interop.Idle(),"completed dependencies stayed live");interop.FreeAll(first);
  {
    vk::Interop refused(device);first.fail_signal=true;const unsigned waits=second.waits;
    Check(refused.OrderQueues(first,second,100u)==vk::WaitResult::FAILED&&second.waits==waits,"failed signal queued a dead wait");
    Check(!refused.RetireQueueDependencies(second),"indeterminate failed signal was prematurely retired");first.fail_signal=false;
    Vk(gpu.idle(gpu.device));refused.FreeAll(second);
  }
  {
    vk::Interop retry(device);second.fail_wait=true;Check(retry.OrderQueues(first,second,100u)==vk::WaitResult::FAILED,"wait rejection was ignored");
    second.fail_wait=false;Check(retry.RetireQueueDependencies(second),"failed wait could not be retired after retry");
    Vk(gpu.idle(gpu.device));retry.FreeFinished(second);Check(retry.Idle(),"retry leaked a semaphore");retry.FreeAll(second);
  }
  device.record.timeline=false;vk::Interop fallback(device);
  Check(fallback.OrderQueues(first,second,1000u)==vk::WaitResult::DONE,"legacy CPU ordering failed");fallback.FreeAll(first);
  std::cout<<"PASS Vulkan dual-queue ordering, queue-pair retirement, failed signal/wait and CPU fallback\n";
}

int main(){try{{Gpu gpu;Fusion(gpu);Present(gpu);QueueOrder(gpu);}Check(errors.load()==0u,"Vulkan validation errors");return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
