// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file compute_kernel.hpp
/// @brief A compute kernel's bundled resources, the builder that gives a group
///        of them one descriptor pool, extra sets of a kernel's layout, and
///        the one-shot dispatch.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/compute_pipeline.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Device;

/// @brief One compute kernel's resources: its descriptor-set layout, the
///        pipeline built from its SPIR-V, and one descriptor set from a
///        shared pool.
///
/// One member per kernel instead of parallel layout, pipeline and set
/// members. @ref KernelSetBuilder builds it; @ref CommandBatch::dispatch and
/// @ref dispatch run it.
///
/// @code
/// ComputeKernel integrate;  // a member, at a fixed address until build()
/// KernelSetBuilder builder(device);
/// VKC_TRY(builder.add(integrate, "integrate", kIntegrateSpv,
///                     sizeof(kIntegrateSpv), 3, &push_range));
/// VKC_ASSIGN(pool_, builder.build());
/// integrate.set.write_storage_buffer(0, voxels.handle(), 0, VK_WHOLE_SIZE);
/// @endcode
struct ComputeKernel {
  DescriptorSetLayout layout;  ///< Set 0's layout (N storage buffers).
  ComputePipeline pipeline;    ///< Built from the kernel's SPIR-V.
  DescriptorSet set;           ///< One set, from the shared pool.
  /// What the kernel is called, for a GPU capture and for diagnostics.
  /// Borrowed, never copied: a string literal, as every registration passes.
  /// A dispatch wraps its commands in a debug-utils region of this name, so a
  /// capture reads `integrate` rather than an anonymous dispatch.
  const char* name = nullptr;
  /// The push-constant range's size, from offset 0; 0 for none. A dispatch
  /// pushes at most this many bytes.
  std::uint32_t push_bytes = 0;
  /// Its storage-buffer bindings, as given to @ref KernelSetBuilder::add.
  std::uint32_t bindings = 0;

  /// @brief Construct an empty kernel; @ref valid is false.
  ComputeKernel() noexcept = default;
  ~ComputeKernel() = default;
  // Hand-written only to empty the copyable, non-owning `set` on the source:
  // `layout` and `pipeline` empty themselves on a move, but a defaulted move
  // would copy `set`, leaving a moved-from kernel with a stale handle.
  ComputeKernel(ComputeKernel&& other) noexcept
      : layout(std::move(other.layout)),
        pipeline(std::move(other.pipeline)),
        set(std::exchange(other.set, DescriptorSet{})),
        name(std::exchange(other.name, nullptr)),
        push_bytes(std::exchange(other.push_bytes, 0)),
        bindings(std::exchange(other.bindings, 0)) {}
  ComputeKernel& operator=(ComputeKernel&& other) noexcept {
    if (this != &other) {
      layout = std::move(other.layout);
      pipeline = std::move(other.pipeline);
      set = std::exchange(other.set, DescriptorSet{});
      name = std::exchange(other.name, nullptr);
      push_bytes = std::exchange(other.push_bytes, 0);
      bindings = std::exchange(other.bindings, 0);
    }
    return *this;
  }
  ComputeKernel(const ComputeKernel&) = delete;
  ComputeKernel& operator=(const ComputeKernel&) = delete;

  /// @return Whether it is fully built: its pipeline made and its set
  ///         allocated. `false` before @ref KernelSetBuilder::build, and
  ///         when moved-from.
  bool valid() const noexcept { return pipeline.valid() && set.valid(); }
};

/// @brief Builds a group of @ref ComputeKernel that share one descriptor pool.
///
/// @ref add each kernel -- its SPIR-V, storage-buffer count and optional
/// push-constant range -- then @ref build sizes the pool to the exact
/// descriptor total, creates it, and allocates every kernel's set. That
/// resolves the pool's chicken-and-egg: sizing it needs every kernel's
/// bindings, and each set must come from it. Every binding is a compute-stage
/// storage buffer.
///
/// TODO: take other descriptor types (storage images, uniform buffers) when a
/// kernel needs them; until then such a kernel builds its own layout and
/// @ref ComputePipeline, and fills a @ref ComputeKernel by hand.
///
/// @code
/// KernelSetBuilder builder(device);
/// VKC_TRY(builder.add(clear, "clear", kClearSpv, sizeof(kClearSpv), 1));
/// VKC_TRY(builder.add(integrate, "integrate", kIntegrateSpv,
///                     sizeof(kIntegrateSpv), 3, &push_range));
/// VKC_ASSIGN(DescriptorPool pool, builder.build());  // outlives the sets
/// @endcode
class VKC_VULKAN_API KernelSetBuilder {
 public:
  /// @brief Start a group of kernels on @p device, with none registered.
  /// @param device  The device the kernels are built on; borrowed, and it
  ///                must outlive the builder. A @ref Device rather than a
  ///                `VkDevice`, as only it can name the objects for a
  ///                profiler.
  explicit KernelSetBuilder(const Device& device) noexcept : device_(&device) {}

  KernelSetBuilder(const KernelSetBuilder&) = delete;
  KernelSetBuilder& operator=(const KernelSetBuilder&) = delete;
  KernelSetBuilder(KernelSetBuilder&&) noexcept = default;
  KernelSetBuilder& operator=(KernelSetBuilder&&) noexcept = default;
  ~KernelSetBuilder() = default;

  /// @brief Register a kernel: build @p out's layout (@p bindings storage
  ///        buffers at 0 .. bindings - 1) and its pipeline now; @ref build
  ///        allocates its set.
  ///
  /// Everything is built before @p out is touched: on success @p out is
  /// replaced whole -- a kernel built before loses its old layout, pipeline
  /// and set, and is not @ref ComputeKernel::valid until @ref build -- and on
  /// failure it is left as it was.
  ///
  /// @warning The builder keeps a pointer to @p out until @ref build, so each
  ///          registered kernel stays at a fixed address from here through
  ///          @ref build -- a member, or in a container reserved in advance --
  ///          and outlives the builder and the pool @ref build returns.
  /// @param out       Receives the layout and pipeline, later the set.
  /// @param name      What the kernel is called, on @ref ComputeKernel::name's
  ///                  terms (a string literal). It names the kernel's capture
  ///                  region, its pipeline and layouts, and any failure here.
  /// @param spv       The SPIR-V, 4-byte aligned (as `vkc_embed_shaders`
  ///                  emits it).
  /// @param spv_size  Its size in bytes.
  /// @param bindings  The storage buffers the shader declares in set 0.
  /// @param push      The push-constant range, or null for none; it starts
  ///                  at offset 0, where a dispatch pushes.
  /// @return OK; @ref Status::Code::InvalidArgument for a push range off
  ///         offset 0; or the layout's, shader's or pipeline's failure, each
  ///         prefixed with @p name, as one `create` registers several kernels
  ///         and the failure itself names only the Vulkan call.
  Status add(ComputeKernel& out, const char* name, const unsigned char* spv,
             std::size_t spv_size, std::uint32_t bindings,
             const VkPushConstantRange* push = nullptr);

  /// @brief Create the shared pool, sized to every registered kernel, and
  ///        allocate each kernel's set from it.
  ///
  /// Every set is allocated before any is handed out, so on failure no
  /// kernel holds a set of a pool that is gone.
  /// @return The pool, which the caller owns and keeps while the sets are in
  ///         use; or the pool's or a set's failure.
  Result<DescriptorPool> build();

 private:
  const Device* device_;
  std::vector<ComputeKernel*> kernels_;
  std::uint32_t descriptor_total_ = 0;
};

/// @brief Descriptor sets of one kernel's layout, in a pool of their own.
///
/// A @ref CommandBatch binds a set when it submits, so it refuses one
/// rewritten after its dispatch was recorded, or freed. A batch that
/// dispatches one kernel several times over different buffers -- each camera
/// of a rig in one submit -- binds one of these to each dispatch (the set
/// overload of @ref CommandBatch::dispatch).
///
/// @code
/// KernelSets per_camera;
/// VKC_TRY(per_camera.reserve(device, undistort, camera_count));
/// for (std::uint32_t c = 0; c < camera_count; ++c) {
///   per_camera[c].write_storage_buffer(0, frames[c].handle(), 0,
///                                      VK_WHOLE_SIZE);
///   VKC_TRY(batch.dispatch(undistort, per_camera[c], &push, sizeof(push),
///                          groups, max_groups));
/// }
/// @endcode
class VKC_VULKAN_API KernelSets {
 public:
  /// @brief Construct with no sets.
  KernelSets() noexcept = default;
  KernelSets(KernelSets&& other) noexcept
      : pool_(std::move(other.pool_)), sets_(std::exchange(other.sets_, {})) {}
  KernelSets& operator=(KernelSets&& other) noexcept {
    if (this != &other) {
      pool_ = std::move(other.pool_);
      sets_ = std::exchange(other.sets_, {});
    }
    return *this;
  }
  KernelSets(const KernelSets&) = delete;
  KernelSets& operator=(const KernelSets&) = delete;
  ~KernelSets() = default;

  /// @brief Hold at least @p count sets of @p kernel's layout.
  ///
  /// Grow-only: holding fewer, the pool is replaced by one of @p count sets,
  /// which frees every set held before -- a @ref CommandBatch that recorded a
  /// dispatch on one then refuses to submit. New sets are unwritten.
  /// @param device  The kernel's device.
  /// @param kernel  A built kernel, the same one on every call.
  /// @param count   How many sets.
  /// @return OK; @ref Status::Code::InvalidArgument for an unbuilt kernel or
  ///         a zero @p count; or the pool's failure, which keeps the sets
  ///         held.
  Status reserve(const Device& device, const ComputeKernel& kernel,
                 std::uint32_t count);

  /// @return How many sets are held.
  std::size_t size() const noexcept { return sets_.size(); }
  /// @return The @p i-th set. @pre `i < size()`.
  const DescriptorSet& operator[](std::size_t i) const noexcept {
    return sets_[i];
  }

 private:
  DescriptorPool pool_;
  std::vector<DescriptorSet> sets_;
};

/// @brief Record and submit a one-shot 1-D dispatch of @p kernel over
///        @p groups workgroups: a @ref CommandBatch of that one command.
///
/// So it binds, pushes, dispatches and makes the writes visible to the next
/// dispatch, the host and a renderer exactly as a batch does, and refuses
/// what @ref CommandBatch::dispatch refuses.
///
/// TODO: V4 adds the optional `GpuStageScope*` span recon's takes.
///
/// @code
/// const Push push{count, delta};
/// VKC_TRY(dispatch(device, add, &push, sizeof(push), group_count(count, 64),
///                  device.caps().limits().maxComputeWorkGroupCount[0]));
/// @endcode
/// @param device      The kernel's device.
/// @param kernel      A built kernel whose set is written.
/// @param push        Push-constant bytes; null only when @p push_size is 0.
/// @param push_size   A multiple of 4, at most the kernel's
///                    @ref ComputeKernel::push_bytes.
/// @param groups      Workgroups along x.
/// @param max_groups  The device's `maxComputeWorkGroupCount[0]`.
/// @return OK; @ref Status::Code::InvalidArgument for @p groups past
///         @p max_groups, a bad push, or an unbuilt kernel; or the submit's
///         failure.
VKC_VULKAN_API Status dispatch(const Device& device,
                               const ComputeKernel& kernel, const void* push,
                               std::uint32_t push_size, std::uint32_t groups,
                               std::uint32_t max_groups);

}  // namespace volumetric_kit::core
