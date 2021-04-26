// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vulkan_surface.h"

#include <lib/async/default.h>
#include <lib/ui/scenic/cpp/commands.h>

#include <algorithm>

#include "flutter/fml/trace_event.h"
#include "runtime/dart/utils/inlines.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/gpu/GrBackendSemaphore.h"
#include "third_party/skia/include/gpu/GrBackendSurface.h"
#include "third_party/skia/include/gpu/GrDirectContext.h"

#define LOG_AND_RETURN(cond, msg) \
  if (cond) {                     \
    FML_DLOG(ERROR) << msg;       \
    return false;                 \
  }

namespace flutter_runner {

namespace {

constexpr SkColorType kSkiaColorType = kRGBA_8888_SkColorType;
constexpr VkFormat kVulkanFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageCreateFlags kVulkanImageCreateFlags = 0;
// TODO: We should only keep usages that are actually required by Skia.
constexpr VkImageUsageFlags kVkImageUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

}  // namespace

bool VulkanSurface::CreateVulkanImage(vulkan::VulkanProvider& vulkan_provider,
                                      const SkISize& size,
                                      VulkanImage* out_vulkan_image) {
  TRACE_EVENT0("flutter", "CreateVulkanImage");

  FML_DCHECK(!size.isEmpty());
  FML_DCHECK(out_vulkan_image != nullptr);

  out_vulkan_image->vk_collection_image_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_COLLECTION_IMAGE_CREATE_INFO_FUCHSIA,
      .pNext = nullptr,
      .collection = collection_,
      .index = 0,
  };

  out_vulkan_image->vk_image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &out_vulkan_image->vk_collection_image_create_info,
      .flags = kVulkanImageCreateFlags,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = kVulkanFormat,
      .extent = VkExtent3D{static_cast<uint32_t>(size.width()),
                           static_cast<uint32_t>(size.height()), 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = kVkImageUsage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = nullptr,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  if (VK_CALL_LOG_ERROR(
          vulkan_provider.vk().SetBufferCollectionConstraintsFUCHSIA(
              vulkan_provider.vk_device(), collection_,
              &out_vulkan_image->vk_image_create_info)) != VK_SUCCESS) {
    return false;
  }

  {
    VkImage vk_image = VK_NULL_HANDLE;
    if (VK_CALL_LOG_ERROR(vulkan_provider.vk().CreateImage(
            vulkan_provider.vk_device(),
            &out_vulkan_image->vk_image_create_info, nullptr, &vk_image)) !=
        VK_SUCCESS) {
      return false;
    }

    out_vulkan_image->vk_image = {
        vk_image, [&vulkan_provider = vulkan_provider](VkImage image) {
          vulkan_provider.vk().DestroyImage(vulkan_provider.vk_device(), image,
                                            NULL);
        }};
  }

  vulkan_provider.vk().GetImageMemoryRequirements(
      vulkan_provider.vk_device(), out_vulkan_image->vk_image,
      &out_vulkan_image->vk_memory_requirements);

  return true;
}

VulkanSurface::VulkanSurface(
    vulkan::VulkanProvider& vulkan_provider,
    fuchsia::sysmem::AllocatorSyncPtr& sysmem_allocator,
    sk_sp<GrDirectContext> context,
    scenic::Session* session,
    const SkISize& size,
    uint32_t buffer_id)
    : vulkan_provider_(vulkan_provider), session_(session), wait_(this) {
  FML_DCHECK(session_);

  if (!AllocateDeviceMemory(sysmem_allocator, std::move(context), size,
                            buffer_id)) {
    FML_DLOG(INFO) << "Could not allocate device memory.";
    return;
  }

  if (!CreateFences()) {
    FML_DLOG(INFO) << "Could not create signal fences.";
    return;
  }

  PushSessionImageSetupOps(session);

  std::fill(size_history_.begin(), size_history_.end(), SkISize::MakeEmpty());

  wait_.set_object(release_event_.get());
  wait_.set_trigger(ZX_EVENT_SIGNALED);
  Reset();

  valid_ = true;
}

VulkanSurface::~VulkanSurface() {
  if (image_id_) {
    session_->Enqueue(scenic::NewReleaseResourceCmd(image_id_));
  }
  if (buffer_id_) {
    session_->DeregisterBufferCollection(buffer_id_);
  }
  wait_.Cancel();
  wait_.set_object(ZX_HANDLE_INVALID);
}

bool VulkanSurface::IsValid() const {
  return valid_;
}

SkISize VulkanSurface::GetSize() const {
  if (!valid_) {
    return SkISize::Make(0, 0);
  }

  return SkISize::Make(sk_surface_->width(), sk_surface_->height());
}

vulkan::VulkanHandle<VkSemaphore> VulkanSurface::SemaphoreFromEvent(
    const zx::event& event) const {
  VkResult result;
  VkSemaphore semaphore;

  zx::event semaphore_event;
  zx_status_t status = event.duplicate(ZX_RIGHT_SAME_RIGHTS, &semaphore_event);
  if (status != ZX_OK) {
    FML_DLOG(ERROR) << "failed to duplicate semaphore event";
    return vulkan::VulkanHandle<VkSemaphore>();
  }

  VkSemaphoreCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
  };

  result = VK_CALL_LOG_ERROR(vulkan_provider_.vk().CreateSemaphore(
      vulkan_provider_.vk_device(), &create_info, nullptr, &semaphore));
  if (result != VK_SUCCESS) {
    return vulkan::VulkanHandle<VkSemaphore>();
  }

  VkImportSemaphoreZirconHandleInfoFUCHSIA import_info = {
      .sType =
          VK_STRUCTURE_TYPE_TEMP_IMPORT_SEMAPHORE_ZIRCON_HANDLE_INFO_FUCHSIA,
      .pNext = nullptr,
      .semaphore = semaphore,
      .handleType =
          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TEMP_ZIRCON_EVENT_BIT_FUCHSIA,
      .handle = static_cast<uint32_t>(semaphore_event.release())};

  result = VK_CALL_LOG_ERROR(
      vulkan_provider_.vk().ImportSemaphoreZirconHandleFUCHSIA(
          vulkan_provider_.vk_device(), &import_info));
  if (result != VK_SUCCESS) {
    return vulkan::VulkanHandle<VkSemaphore>();
  }

  return vulkan::VulkanHandle<VkSemaphore>(
      semaphore, [&vulkan_provider = vulkan_provider_](VkSemaphore semaphore) {
        vulkan_provider.vk().DestroySemaphore(vulkan_provider.vk_device(),
                                              semaphore, nullptr);
      });
}

bool VulkanSurface::CreateFences() {
  if (zx::event::create(0, &acquire_event_) != ZX_OK) {
    return false;
  }

  acquire_semaphore_ = SemaphoreFromEvent(acquire_event_);
  if (!acquire_semaphore_) {
    FML_DLOG(ERROR) << "failed to create acquire semaphore";
    return false;
  }

  if (zx::event::create(0, &release_event_) != ZX_OK) {
    return false;
  }

  command_buffer_fence_ = vulkan_provider_.CreateFence();

  return true;
}

bool VulkanSurface::AllocateDeviceMemory(
    fuchsia::sysmem::AllocatorSyncPtr& sysmem_allocator,
    sk_sp<GrDirectContext> context,
    const SkISize& size,
    uint32_t buffer_id) {
  if (size.isEmpty()) {
    return false;
  }

  fuchsia::sysmem::BufferCollectionTokenSyncPtr vulkan_token;
  zx_status_t status =
      sysmem_allocator->AllocateSharedCollection(vulkan_token.NewRequest());
  LOG_AND_RETURN(status != ZX_OK, "Failed to allocate collection");
  fuchsia::sysmem::BufferCollectionTokenSyncPtr scenic_token;
  status = vulkan_token->Duplicate(std::numeric_limits<uint32_t>::max(),
                                   scenic_token.NewRequest());
  LOG_AND_RETURN(status != ZX_OK, "Failed to duplicate token");
  status = vulkan_token->Sync();
  LOG_AND_RETURN(status != ZX_OK, "Failed to sync token");

  session_->RegisterBufferCollection(buffer_id, std::move(scenic_token));
  buffer_id_ = buffer_id;

  VkBufferCollectionCreateInfoFUCHSIA import_info;
  import_info.collectionToken = vulkan_token.Unbind().TakeChannel().release();
  VkBufferCollectionFUCHSIA collection;
  if (VK_CALL_LOG_ERROR(vulkan_provider_.vk().CreateBufferCollectionFUCHSIA(
          vulkan_provider_.vk_device(), &import_info, nullptr, &collection)) !=
      VK_SUCCESS) {
    return false;
  }

  collection_ = {collection, [&vulkan_provider = vulkan_provider_](
                                 VkBufferCollectionFUCHSIA collection) {
                   vulkan_provider.vk().DestroyBufferCollectionFUCHSIA(
                       vulkan_provider.vk_device(), collection, nullptr);
                 }};

  VulkanImage vulkan_image;
  LOG_AND_RETURN(!CreateVulkanImage(vulkan_provider_, size, &vulkan_image),
                 "Failed to create VkImage");

  vulkan_image_ = std::move(vulkan_image);
  const VkMemoryRequirements& memory_requirements =
      vulkan_image_.vk_memory_requirements;
  VkImageCreateInfo& image_create_info = vulkan_image_.vk_image_create_info;

  VkBufferCollectionPropertiesFUCHSIA properties = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_COLLECTION_PROPERTIES_FUCHSIA};
  if (VK_CALL_LOG_ERROR(
          vulkan_provider_.vk().GetBufferCollectionPropertiesFUCHSIA(
              vulkan_provider_.vk_device(), collection_, &properties)) !=
      VK_SUCCESS) {
    return false;
  }

  VkImportMemoryBufferCollectionFUCHSIA import_memory_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_BUFFER_COLLECTION_FUCHSIA,
      .pNext = nullptr,
      .collection = collection_,
      .index = 0,
  };
  auto bits = memory_requirements.memoryTypeBits & properties.memoryTypeBits;
  FML_DCHECK(bits != 0);
  VkMemoryAllocateInfo allocation_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_memory_info,
      .allocationSize = memory_requirements.size,
      .memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(bits)),
  };

  {
    TRACE_EVENT1("flutter", "vkAllocateMemory", "allocationSize",
                 allocation_info.allocationSize);
    VkDeviceMemory vk_memory = VK_NULL_HANDLE;
    if (VK_CALL_LOG_ERROR(vulkan_provider_.vk().AllocateMemory(
            vulkan_provider_.vk_device(), &allocation_info, NULL,
            &vk_memory)) != VK_SUCCESS) {
      return false;
    }

    vk_memory_ = {vk_memory,
                  [&vulkan_provider = vulkan_provider_](VkDeviceMemory memory) {
                    vulkan_provider.vk().FreeMemory(vulkan_provider.vk_device(),
                                                    memory, NULL);
                  }};

    vk_memory_info_ = allocation_info;
  }

  // Bind image memory.
  if (VK_CALL_LOG_ERROR(vulkan_provider_.vk().BindImageMemory(
          vulkan_provider_.vk_device(), vulkan_image_.vk_image, vk_memory_,
          0)) != VK_SUCCESS) {
    return false;
  }

  return SetupSkiaSurface(std::move(context), size, kSkiaColorType,
                          image_create_info, memory_requirements);
}

bool VulkanSurface::SetupSkiaSurface(sk_sp<GrDirectContext> context,
                                     const SkISize& size,
                                     SkColorType color_type,
                                     const VkImageCreateInfo& image_create_info,
                                     const VkMemoryRequirements& memory_reqs) {
  if (context == nullptr) {
    return false;
  }

  GrVkAlloc alloc;
  alloc.fMemory = vk_memory_;
  alloc.fOffset = 0;
  alloc.fSize = memory_reqs.size;
  alloc.fFlags = 0;

  GrVkImageInfo image_info;
  image_info.fImage = vulkan_image_.vk_image;
  image_info.fAlloc = alloc;
  image_info.fImageTiling = image_create_info.tiling;
  image_info.fImageLayout = image_create_info.initialLayout;
  image_info.fFormat = image_create_info.format;
  image_info.fImageUsageFlags = image_create_info.usage;
  image_info.fSampleCount = 1;
  image_info.fLevelCount = image_create_info.mipLevels;

  GrBackendRenderTarget sk_render_target(size.width(), size.height(), 0,
                                         image_info);

  SkSurfaceProps sk_surface_props(0, kUnknown_SkPixelGeometry);

  auto sk_surface =
      SkSurface::MakeFromBackendRenderTarget(context.get(),             //
                                             sk_render_target,          //
                                             kTopLeft_GrSurfaceOrigin,  //
                                             color_type,                //
                                             SkColorSpace::MakeSRGB(),  //
                                             &sk_surface_props          //
      );

  if (!sk_surface || sk_surface->getCanvas() == nullptr) {
    return false;
  }
  sk_surface_ = std::move(sk_surface);

  return true;
}

void VulkanSurface::PushSessionImageSetupOps(scenic::Session* session) {
  if (image_id_ == 0)
    image_id_ = session->AllocResourceId();
  session->Enqueue(scenic::NewCreateImage2Cmd(
      image_id_, sk_surface_->width(), sk_surface_->height(), buffer_id_, 0));
}

uint32_t VulkanSurface::GetImageId() {
  return image_id_;
}

sk_sp<SkSurface> VulkanSurface::GetSkiaSurface() const {
  return valid_ ? sk_surface_ : nullptr;
}

size_t VulkanSurface::AdvanceAndGetAge() {
  size_history_[size_history_index_] = GetSize();
  size_history_index_ = (size_history_index_ + 1) % kSizeHistorySize;
  age_++;
  return age_;
}

bool VulkanSurface::FlushSessionAcquireAndReleaseEvents() {
  zx::event acquire, release;

  if (acquire_event_.duplicate(ZX_RIGHT_SAME_RIGHTS, &acquire) != ZX_OK ||
      release_event_.duplicate(ZX_RIGHT_SAME_RIGHTS, &release) != ZX_OK) {
    return false;
  }

  session_->EnqueueAcquireFence(std::move(acquire));
  session_->EnqueueReleaseFence(std::move(release));
  age_ = 0;
  return true;
}

void VulkanSurface::SignalWritesFinished(
    const std::function<void(void)>& on_writes_committed) {
  FML_DCHECK(on_writes_committed);

  if (!valid_) {
    on_writes_committed();
    return;
  }

  dart_utils::Check(pending_on_writes_committed_ == nullptr,
                    "Attempted to signal a write on the surface when the "
                    "previous write has not yet been acknowledged by the "
                    "compositor.");

  pending_on_writes_committed_ = on_writes_committed;
}

void VulkanSurface::Reset() {
  if (acquire_event_.signal(ZX_EVENT_SIGNALED, 0u) != ZX_OK ||
      release_event_.signal(ZX_EVENT_SIGNALED, 0u) != ZX_OK) {
    valid_ = false;
    FML_DLOG(ERROR)
        << "Could not reset fences. The surface is no longer valid.";
  }

  VkFence fence = command_buffer_fence_;

  if (command_buffer_) {
    VK_CALL_LOG_ERROR(vulkan_provider_.vk().WaitForFences(
        vulkan_provider_.vk_device(), 1, &fence, VK_TRUE, UINT64_MAX));
    command_buffer_.reset();
  }

  VK_CALL_LOG_ERROR(vulkan_provider_.vk().ResetFences(
      vulkan_provider_.vk_device(), 1, &fence));

  // Need to make a new  acquire semaphore every frame or else validation layers
  // get confused about why no one is waiting on it in this VkInstance
  acquire_semaphore_.Reset();
  acquire_semaphore_ = SemaphoreFromEvent(acquire_event_);
  if (!acquire_semaphore_) {
    FML_DLOG(ERROR) << "failed to create acquire semaphore";
  }

  wait_.Begin(async_get_default_dispatcher());

  // It is safe for the caller to collect the surface in the callback.
  auto callback = pending_on_writes_committed_;
  pending_on_writes_committed_ = nullptr;
  if (callback) {
    callback();
  }
}

void VulkanSurface::OnHandleReady(async_dispatcher_t* dispatcher,
                                  async::WaitBase* wait,
                                  zx_status_t status,
                                  const zx_packet_signal_t* signal) {
  if (status != ZX_OK)
    return;
  FML_DCHECK(signal->observed & ZX_EVENT_SIGNALED);
  Reset();
}

}  // namespace flutter_runner
