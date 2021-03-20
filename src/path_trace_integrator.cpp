#include "path_trace_integrator.hpp"
#include "utils/cl_exception.hpp"
#include "Scene/scene.hpp"
#include "Scene/camera.hpp"
#include "acceleration_structure.hpp"
#include "Utils/blue_noise_sampler.hpp"
#include <GL/glew.h>

namespace args
{
    namespace Raygen
    {
        enum
        {
            kWidth,
            kHeight,
            kCameraPos,
            kCameraFront,
            kCameraUp,
            kFrameCount,
            kFrameSeed,
            kAperture,
            kFocusDistance,
            // Output
            kRayBuffer,
            kRayCounterBuffer,
            kPixelIndicesBuffer,
            kThroughputsBuffer,
        };
    }

    namespace Miss
    {
        enum
        {
            kRayBuffer,
            kRayCounterBuffer,
            kHitsBuffer,
            kPixelIndicesBuffer,
            kThroughputsBuffer,
            kIblTextureBuffer,
            kRadianceBuffer,
        };
    }

    namespace HitSurface
    {
        enum
        {
            // Input
            kIncomingRayBuffer,
            kIncomingRayCounterBuffer,
            kIncomingPixelIndicesBuffer,
            kHitsBuffer,
            kTrianglesBuffer,
            kMaterialsBuffer,
            kBounce,
            kWidth,
            kHeight,
            kSampleCounterBuffer,
            kSobolBuffer,
            kScramblingTileBuffer,
            kRankingTileBuffer,
            // Output
            kThroughputsBuffer,
            kOutgoingRayBuffer,
            kOutgoingRayCounterBuffer,
            kOutgoingPixelIndicesBuffer,
            kRadianceBuffer,
        };
    }

    namespace Resolve
    {
        enum
        {
            // Input
            kWidth,
            kHeight,
            kRadianceBuffer,
            kSampleCounterBuffer,
            // Output
            kResolvedTexture,
        };
    }
}

PathTraceIntegrator::PathTraceIntegrator(std::uint32_t width, std::uint32_t height,
    CLContext& cl_context, AccelerationStructure& acc_structure, cl_GLuint gl_interop_image)
    : width_(width)
    , height_(height)
    , cl_context_(cl_context)
    , acc_structure_(acc_structure)
    , gl_interop_image_(gl_interop_image)
{
    std::uint32_t num_rays = width_ * height_;

    // Create buffers and images
    cl_int status;

    radiance_buffer_ = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_WRITE,
        width_ * height_ * sizeof(cl_float4), nullptr, &status);

    for (int i = 0; i < 2; ++i)
    {
        rays_buffer_[i] = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_WRITE,
            num_rays * sizeof(Ray), nullptr, &status);

        pixel_indices_buffer_[i] = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_WRITE,
            num_rays * sizeof(std::uint32_t), nullptr, &status);

        ray_counter_buffer_[i] = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_WRITE,
            sizeof(std::uint32_t), nullptr, &status);
    }

    hits_buffer_ = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_WRITE,
        num_rays * sizeof(Hit), nullptr, &status);

    throughputs_buffer_ = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_WRITE,
        num_rays * sizeof(cl_float3), nullptr, &status);

    if (status != CL_SUCCESS)
    {
        throw CLException("Failed to create radiance buffer", status);
    }

    sample_counter_buffer_ = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_WRITE,
        sizeof(std::uint32_t), nullptr, &status);

    // Sampler buffers
    sampler_sobol_buffer_ = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        sizeof(sobol_256spp_256d), (void*)sobol_256spp_256d, &status);

    sampler_scrambling_tile_buffer_ = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        sizeof(scramblingTile), (void*)scramblingTile, &status);

    sampler_ranking_tile_buffer_ = cl::Buffer(cl_context.GetContext(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        sizeof(rankingTile), (void*)rankingTile, &status);

    output_image_ = std::make_unique<cl::ImageGL>(cl_context.GetContext(), CL_MEM_WRITE_ONLY,
        GL_TEXTURE_2D, 0, gl_interop_image_, &status);

    if (status != CL_SUCCESS)
    {
        throw CLException("Failed to create output image", status);
    }

    // Create kernels
    reset_kernel_ = std::make_unique<CLKernel>("src/Kernels/reset_radiance.cl", cl_context_);
    raygen_kernel_ = std::make_unique<CLKernel>("src/Kernels/raygeneration.cl", cl_context_);
    miss_kernel_ = std::make_unique<CLKernel>("src/Kernels/miss.cl", cl_context_);
    hit_surface_kernel_ = std::make_unique<CLKernel>("src/Kernels/hit_surface.cl", cl_context_);
    clear_counter_kernel_ = std::make_unique<CLKernel>("src/Kernels/clear_counter.cl", cl_context_);
    increment_counter_kernel_ = std::make_unique<CLKernel>("src/Kernels/increment_counter.cl", cl_context_);
    resolve_kernel_ = std::make_unique<CLKernel>("src/Kernels/resolve_radiance.cl", cl_context_);

    // Setup kernels
    cl_mem output_image_mem = (*output_image_)();

    // Setup reset kernel
    reset_kernel_->SetArgument(0, &width_, sizeof(width_));
    reset_kernel_->SetArgument(1, &height_, sizeof(height_));
    reset_kernel_->SetArgument(2, &radiance_buffer_, sizeof(radiance_buffer_));

    // Setup raygen kernel
    raygen_kernel_->SetArgument(args::Raygen::kWidth, &width_, sizeof(width_));
    raygen_kernel_->SetArgument(args::Raygen::kHeight, &height_, sizeof(height_));
    raygen_kernel_->SetArgument(args::Raygen::kRayBuffer, &rays_buffer_[0], sizeof(rays_buffer_[0]));
    raygen_kernel_->SetArgument(args::Raygen::kRayCounterBuffer, &ray_counter_buffer_[0], sizeof(ray_counter_buffer_[0]));
    raygen_kernel_->SetArgument(args::Raygen::kPixelIndicesBuffer, &pixel_indices_buffer_[0], sizeof(pixel_indices_buffer_[0]));
    raygen_kernel_->SetArgument(args::Raygen::kThroughputsBuffer, &throughputs_buffer_, sizeof(throughputs_buffer_));

    // Setup miss kernel
    miss_kernel_->SetArgument(args::Miss::kHitsBuffer, &hits_buffer_, sizeof(hits_buffer_));
    miss_kernel_->SetArgument(args::Miss::kThroughputsBuffer, &throughputs_buffer_, sizeof(throughputs_buffer_));
    miss_kernel_->SetArgument(args::Miss::kRadianceBuffer, &radiance_buffer_, sizeof(radiance_buffer_));

    // Setup hit surface kernel
    hit_surface_kernel_->SetArgument(args::HitSurface::kHitsBuffer,
        &hits_buffer_, sizeof(hits_buffer_));
    hit_surface_kernel_->SetArgument(args::HitSurface::kThroughputsBuffer,
        &throughputs_buffer_, sizeof(throughputs_buffer_));
    hit_surface_kernel_->SetArgument(args::HitSurface::kRadianceBuffer,
        &radiance_buffer_, sizeof(radiance_buffer_));

    hit_surface_kernel_->SetArgument(args::HitSurface::kWidth,
        &width_, sizeof(width_));
    hit_surface_kernel_->SetArgument(args::HitSurface::kHeight,
        &height_, sizeof(height_));

    hit_surface_kernel_->SetArgument(args::HitSurface::kSampleCounterBuffer,
        &sample_counter_buffer_, sizeof(sample_counter_buffer_));

    hit_surface_kernel_->SetArgument(args::HitSurface::kSobolBuffer,
        &sampler_sobol_buffer_, sizeof(sampler_sobol_buffer_));
    hit_surface_kernel_->SetArgument(args::HitSurface::kScramblingTileBuffer,
        &sampler_scrambling_tile_buffer_, sizeof(sampler_scrambling_tile_buffer_));
    hit_surface_kernel_->SetArgument(args::HitSurface::kRankingTileBuffer,
        &sampler_ranking_tile_buffer_, sizeof(sampler_ranking_tile_buffer_));

    // Setup resolve kernel
    resolve_kernel_->SetArgument(args::Resolve::kWidth, &width_, sizeof(width_));
    resolve_kernel_->SetArgument(args::Resolve::kHeight, &height_, sizeof(height_));
    resolve_kernel_->SetArgument(args::Resolve::kSampleCounterBuffer, &sample_counter_buffer_, sizeof(sample_counter_buffer_));
    resolve_kernel_->SetArgument(args::Resolve::kRadianceBuffer, &radiance_buffer_, sizeof(radiance_buffer_));
    resolve_kernel_->SetArgument(args::Resolve::kResolvedTexture, &output_image_mem, sizeof(output_image_mem));


    // Don't forget to reset frame index
    Reset();
}

void PathTraceIntegrator::Reset()
{
    // Reset frame index
    clear_counter_kernel_->SetArgument(0, &sample_counter_buffer_,
        sizeof(sample_counter_buffer_));
    cl_context_.ExecuteKernel(*clear_counter_kernel_, 1);

    // Reset radiance buffer
    cl_context_.ExecuteKernel(*reset_kernel_, width_ * height_);
}

void PathTraceIntegrator::SetCameraData(Camera const& camera)
{
    float3 origin = camera.GetOrigin();
    float3 front = camera.GetFrontVector();

    float3 right = Cross(front, camera.GetUpVector()).Normalize();
    float3 up = Cross(right, front);
    raygen_kernel_->SetArgument(args::Raygen::kCameraPos, &origin, sizeof(origin));
    raygen_kernel_->SetArgument(args::Raygen::kCameraFront, &front, sizeof(front));
    raygen_kernel_->SetArgument(args::Raygen::kCameraUp, &up, sizeof(up));

    ///@TODO: move frame count to here
    std::uint32_t frame_count = camera.GetFrameCount();
    raygen_kernel_->SetArgument(args::Raygen::kFrameCount, &frame_count, sizeof(frame_count));
    unsigned int seed = rand();
    raygen_kernel_->SetArgument(args::Raygen::kFrameSeed, &seed, sizeof(seed));

    float aperture = camera.GetAperture();
    float focus_distance = camera.GetFocusDistance();
    raygen_kernel_->SetArgument(args::Raygen::kAperture, &aperture, sizeof(aperture));
    raygen_kernel_->SetArgument(args::Raygen::kFocusDistance, &focus_distance, sizeof(focus_distance));
}

void PathTraceIntegrator::SetSceneData(Scene const& scene)
{
    // Set scene buffers
    cl_mem triangle_buffer = scene.GetTriangleBuffer();
    cl_mem material_buffer = scene.GetMaterialBuffer();
    cl_mem env_texture = scene.GetEnvTextureBuffer();

    hit_surface_kernel_->SetArgument(args::HitSurface::kTrianglesBuffer, &triangle_buffer, sizeof(cl_mem));
    hit_surface_kernel_->SetArgument(args::HitSurface::kMaterialsBuffer, &material_buffer, sizeof(cl_mem));
    miss_kernel_->SetArgument(args::Miss::kIblTextureBuffer, &env_texture, sizeof(cl_mem));
}

void PathTraceIntegrator::AdvanceSampleCount()
{
    increment_counter_kernel_->SetArgument(0, &sample_counter_buffer_,
        sizeof(sample_counter_buffer_));
    cl_context_.ExecuteKernel(*increment_counter_kernel_, 1);
}

void PathTraceIntegrator::GenerateRays()
{
    std::uint32_t num_rays = width_ * height_;
    cl_context_.ExecuteKernel(*raygen_kernel_, num_rays);
}

void PathTraceIntegrator::IntersectRays(std::uint32_t bounce)
{
    std::uint32_t max_num_rays = width_ * height_;
    std::uint32_t incoming_idx = bounce & 1;

    acc_structure_.IntersectRays(rays_buffer_[incoming_idx], ray_counter_buffer_[incoming_idx],
        max_num_rays, hits_buffer_);
}

void PathTraceIntegrator::ShadeMissedRays(std::uint32_t bounce)
{
    std::uint32_t max_num_rays = width_ * height_;
    std::uint32_t incoming_idx = bounce & 1;

    miss_kernel_->SetArgument(args::Miss::kRayBuffer,
        &rays_buffer_[incoming_idx], sizeof(rays_buffer_[incoming_idx]));
    miss_kernel_->SetArgument(args::Miss::kPixelIndicesBuffer,
        &pixel_indices_buffer_[incoming_idx], sizeof(pixel_indices_buffer_[incoming_idx]));
    miss_kernel_->SetArgument(args::Miss::kRayCounterBuffer,
        &ray_counter_buffer_[incoming_idx], sizeof(ray_counter_buffer_[incoming_idx]));
    cl_context_.ExecuteKernel(*miss_kernel_, max_num_rays);
}

void PathTraceIntegrator::ShadeSurfaceHits(std::uint32_t bounce)
{
    std::uint32_t max_num_rays = width_ * height_;

    std::uint32_t incoming_idx = bounce & 1;
    std::uint32_t outgoing_idx = (bounce + 1) & 1;

    // Incoming rays
    hit_surface_kernel_->SetArgument(args::HitSurface::kIncomingRayBuffer,
        &rays_buffer_[incoming_idx], sizeof(rays_buffer_[incoming_idx]));
    hit_surface_kernel_->SetArgument(args::HitSurface::kIncomingPixelIndicesBuffer,
        &pixel_indices_buffer_[incoming_idx], sizeof(pixel_indices_buffer_[incoming_idx]));
    hit_surface_kernel_->SetArgument(args::HitSurface::kIncomingRayCounterBuffer,
        &ray_counter_buffer_[incoming_idx], sizeof(ray_counter_buffer_[incoming_idx]));
    // Outgoing rays
    hit_surface_kernel_->SetArgument(args::HitSurface::kOutgoingRayBuffer,
        &rays_buffer_[outgoing_idx], sizeof(rays_buffer_[outgoing_idx]));
    hit_surface_kernel_->SetArgument(args::HitSurface::kOutgoingPixelIndicesBuffer,
        &pixel_indices_buffer_[outgoing_idx], sizeof(pixel_indices_buffer_[outgoing_idx]));
    hit_surface_kernel_->SetArgument(args::HitSurface::kOutgoingRayCounterBuffer,
        &ray_counter_buffer_[outgoing_idx], sizeof(ray_counter_buffer_[outgoing_idx]));
    // Other data
    hit_surface_kernel_->SetArgument(args::HitSurface::kBounce, &bounce, sizeof(bounce));

    cl_context_.ExecuteKernel(*hit_surface_kernel_, max_num_rays);
}

void PathTraceIntegrator::ClearOutgoingRayCounter(std::uint32_t bounce)
{
    std::uint32_t outgoing_idx = (bounce + 1) & 1;

    clear_counter_kernel_->SetArgument(0, &ray_counter_buffer_[outgoing_idx],
        sizeof(ray_counter_buffer_[outgoing_idx]));
    cl_context_.ExecuteKernel(*clear_counter_kernel_, 1);
}

void PathTraceIntegrator::ResolveRadiance()
{
    // Copy radiance to the interop image
    cl_context_.AcquireGLObject((*output_image_)());
    cl_context_.ExecuteKernel(*resolve_kernel_, width_ * height_);
    cl_context_.Finish();
    cl_context_.ReleaseGLObject((*output_image_)());
}

void PathTraceIntegrator::Integrate()
{
    GenerateRays();

    for (std::uint32_t bounce = 0; bounce < max_bounces_; ++bounce)
    {
        IntersectRays(bounce);
        ShadeMissedRays(bounce);
        ClearOutgoingRayCounter(bounce);
        ShadeSurfaceHits(bounce);
    }

    AdvanceSampleCount();
    ResolveRadiance();
}