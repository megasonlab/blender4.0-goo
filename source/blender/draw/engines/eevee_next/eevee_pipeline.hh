/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Shading passes contain drawcalls specific to shading pipelines.
 * They are shared across views.
 * This file is only for shading passes. Other passes are declared in their own module.
 */

#pragma once

#include "DRW_render.h"
#include "draw_shader_shared.h"

#include "eevee_lut.hh"
#include "eevee_subsurface.hh"

namespace blender::eevee {

class Instance;
struct RayTraceBuffer;

/* -------------------------------------------------------------------- */
/** \name World Background Pipeline
 *
 * Render world background values.
 * \{ */

class BackgroundPipeline {
 private:
  Instance &inst_;

  PassSimple world_ps_ = {"World.Background"};

 public:
  BackgroundPipeline(Instance &inst) : inst_(inst){};

  void sync(GPUMaterial *gpumat, float background_opacity);
  void render(View &view);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name World Probe Pipeline
 *
 * Renders a single side for the world reflection probe.
 * \{ */

class WorldPipeline {
 private:
  Instance &inst_;

  /* Dummy textures: required to reuse background shader and avoid another shader variation. */
  Texture dummy_renderpass_tx_;
  Texture dummy_cryptomatte_tx_;
  Texture dummy_aov_color_tx_;
  Texture dummy_aov_value_tx_;

  PassSimple cubemap_face_ps_ = {"World.Probe"};

 public:
  WorldPipeline(Instance &inst) : inst_(inst){};

  void sync(GPUMaterial *gpumat);
  void render(View &view);

};  // namespace blender::eevee

/** \} */

/* -------------------------------------------------------------------- */
/** \name World Volume Pipeline
 *
 * \{ */

class WorldVolumePipeline {
 private:
  Instance &inst_;
  bool is_valid_;

  PassSimple world_ps_ = {"World.Volume"};

 public:
  WorldVolumePipeline(Instance &inst) : inst_(inst){};

  void sync(GPUMaterial *gpumat);
  void render(View &view);

  bool is_valid()
  {
    return is_valid_;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Pass
 *
 * \{ */

class ShadowPipeline {
 private:
  Instance &inst_;

  PassMain surface_ps_ = {"Shadow.Surface"};

 public:
  ShadowPipeline(Instance &inst) : inst_(inst){};

  PassMain::Sub *surface_material_add(GPUMaterial *gpumat);

  void sync();
  void render(View &view);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Forward Pass
 *
 * Handles alpha blended surfaces and NPR materials (using Closure to RGBA).
 * \{ */

class ForwardPipeline {
 private:
  Instance &inst_;

  PassMain prepass_ps_ = {"Prepass"};
  PassMain::Sub *prepass_single_sided_static_ps_ = nullptr;
  PassMain::Sub *prepass_single_sided_moving_ps_ = nullptr;
  PassMain::Sub *prepass_double_sided_static_ps_ = nullptr;
  PassMain::Sub *prepass_double_sided_moving_ps_ = nullptr;

  PassMain opaque_ps_ = {"Shading"};
  PassMain::Sub *opaque_single_sided_ps_ = nullptr;
  PassMain::Sub *opaque_double_sided_ps_ = nullptr;

  PassSortable transparent_ps_ = {"Forward.Transparent"};
  float3 camera_forward_;

  // GPUTexture *input_screen_radiance_tx_ = nullptr;

 public:
  ForwardPipeline(Instance &inst) : inst_(inst){};

  void sync();

  PassMain::Sub *prepass_opaque_add(::Material *blender_mat, GPUMaterial *gpumat, bool has_motion);
  PassMain::Sub *material_opaque_add(::Material *blender_mat, GPUMaterial *gpumat);

  PassMain::Sub *prepass_transparent_add(const Object *ob,
                                         ::Material *blender_mat,
                                         GPUMaterial *gpumat);
  PassMain::Sub *material_transparent_add(const Object *ob,
                                          ::Material *blender_mat,
                                          GPUMaterial *gpumat);

  void render(View &view,
              Framebuffer &prepass_fb,
              Framebuffer &combined_fb,
              GPUTexture *combined_tx);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred lighting.
 * \{ */

class DeferredLayer {
 private:
  Instance &inst_;

  PassMain prepass_ps_ = {"Prepass"};
  PassMain::Sub *prepass_single_sided_static_ps_ = nullptr;
  PassMain::Sub *prepass_single_sided_moving_ps_ = nullptr;
  PassMain::Sub *prepass_double_sided_static_ps_ = nullptr;
  PassMain::Sub *prepass_double_sided_moving_ps_ = nullptr;

  PassMain gbuffer_ps_ = {"Shading"};
  PassMain::Sub *gbuffer_single_sided_ps_ = nullptr;
  PassMain::Sub *gbuffer_double_sided_ps_ = nullptr;

  /* Evaluate all light objects contribution. */
  PassSimple eval_light_ps_ = {"EvalLights"};
  /* Combine direct and indirect light contributions and apply BSDF color. */
  PassSimple combine_ps_ = {"Combine"};

  /* Closures bits from the materials in this pass. */
  eClosureBits closure_bits_ = CLOSURE_NONE;

  /**
   * Accumulation textures for all stages of lighting evaluation (Light, SSR, SSSS, SSGI ...).
   * These are split and separate from the main radiance buffer in order to accumulate light for
   * the render passes and avoid too much bandwidth waste. Otherwise, we would have to load the
   * BSDF color and do additive blending for each of the lighting step.
   *
   * NOTE: Not to be confused with the render passes.
   */
  TextureFromPool direct_diffuse_tx_ = {"direct_diffuse_tx"};
  TextureFromPool direct_reflect_tx_ = {"direct_reflect_tx"};
  TextureFromPool direct_refract_tx_ = {"direct_refract_tx"};
  /* Reference to ray-tracing result. */
  GPUTexture *indirect_diffuse_tx_ = nullptr;
  GPUTexture *indirect_reflect_tx_ = nullptr;
  GPUTexture *indirect_refract_tx_ = nullptr;

  Texture radiance_behind_tx_ = {"radiance_behind_tx"};
  Texture radiance_feedback_tx_ = {"radiance_feedback_tx"};
  float4x4 radiance_feedback_persmat_;

 public:
  DeferredLayer(Instance &inst) : inst_(inst){};

  void begin_sync();
  void end_sync();

  PassMain::Sub *prepass_add(::Material *blender_mat, GPUMaterial *gpumat, bool has_motion);
  PassMain::Sub *material_add(::Material *blender_mat, GPUMaterial *gpumat);

  void render(View &main_view,
              View &render_view,
              Framebuffer &prepass_fb,
              Framebuffer &combined_fb,
              int2 extent,
              RayTraceBuffer &rt_buffer,
              bool is_first_pass);
};

class DeferredPipeline {
 private:
  /* Gbuffer filling passes. We could have an arbitrary number of them but for now we just have
   * a hardcoded number of them. */
  DeferredLayer opaque_layer_;
  DeferredLayer refraction_layer_;
  DeferredLayer volumetric_layer_;

 public:
  DeferredPipeline(Instance &inst)
      : opaque_layer_(inst), refraction_layer_(inst), volumetric_layer_(inst){};

  void begin_sync();
  void end_sync();

  PassMain::Sub *prepass_add(::Material *material, GPUMaterial *gpumat, bool has_motion);
  PassMain::Sub *material_add(::Material *material, GPUMaterial *gpumat);

  void render(View &main_view,
              View &render_view,
              Framebuffer &prepass_fb,
              Framebuffer &combined_fb,
              int2 extent,
              RayTraceBuffer &rt_buffer_opaque_layer,
              RayTraceBuffer &rt_buffer_refract_layer);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume Pass
 *
 * \{ */

class VolumePipeline {
 private:
  Instance &inst_;

  PassMain volume_ps_ = {"Volume.Objects"};

 public:
  VolumePipeline(Instance &inst) : inst_(inst){};

  PassMain::Sub *volume_material_add(GPUMaterial *gpumat);

  void sync();
  void render(View &view);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred Probe Capture.
 * \{ */
class DeferredProbeLayer {
 private:
  Instance &inst_;

  PassMain prepass_ps_ = {"Prepass"};
  PassMain::Sub *prepass_single_sided_ps_ = nullptr;
  PassMain::Sub *prepass_double_sided_ps_ = nullptr;

  PassMain gbuffer_ps_ = {"Shading"};
  PassMain::Sub *gbuffer_single_sided_ps_ = nullptr;
  PassMain::Sub *gbuffer_double_sided_ps_ = nullptr;

  PassSimple eval_light_ps_ = {"EvalLights"};

  /* Closures bits from the materials in this pass. */
  eClosureBits closure_bits_;

  Texture dummy_light_tx_ = {"dummy_light_accum_tx"};

 public:
  DeferredProbeLayer(Instance &inst) : inst_(inst){};

  void begin_sync();
  void end_sync();

  PassMain::Sub *prepass_add(::Material *blender_mat, GPUMaterial *gpumat);
  PassMain::Sub *material_add(::Material *blender_mat, GPUMaterial *gpumat);

  void render(View &view, Framebuffer &prepass_fb, Framebuffer &combined_fb, int2 extent);
};

class DeferredProbePipeline {
 private:
  DeferredProbeLayer opaque_layer_;

 public:
  DeferredProbePipeline(Instance &inst) : opaque_layer_(inst){};

  void begin_sync();
  void end_sync();

  PassMain::Sub *prepass_add(::Material *material, GPUMaterial *gpumat);
  PassMain::Sub *material_add(::Material *material, GPUMaterial *gpumat);

  void render(View &view, Framebuffer &prepass_fb, Framebuffer &combined_fb, int2 extent);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Capture Pipeline
 *
 * \{ */

class CapturePipeline {
 private:
  Instance &inst_;

  PassMain surface_ps_ = {"Capture.Surface"};

 public:
  CapturePipeline(Instance &inst) : inst_(inst){};

  PassMain::Sub *surface_material_add(GPUMaterial *gpumat);

  void sync();
  void render(View &view);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utility texture
 *
 * 64x64 2D array texture containing LUT tables and blue noises.
 * \{ */

class UtilityTexture : public Texture {
  struct Layer {
    float4 data[UTIL_TEX_SIZE][UTIL_TEX_SIZE];
  };

  static constexpr int lut_size = UTIL_TEX_SIZE;
  static constexpr int lut_size_sqr = lut_size * lut_size;
  static constexpr int layer_count = UTIL_BTDF_LAYER + 1 + UTIL_BTDF_LAYER_COUNT;

 public:
  UtilityTexture()
      : Texture("UtilityTx",
                GPU_RGBA16F,
                GPU_TEXTURE_USAGE_SHADER_READ,
                int2(lut_size),
                layer_count,
                nullptr)
  {
    Vector<Layer> data(layer_count);
    {
      Layer &layer = data[UTIL_BLUE_NOISE_LAYER];
      memcpy(layer.data, lut::blue_noise, sizeof(layer));
    }
    {
      Layer &layer = data[UTIL_SSS_TRANSMITTANCE_PROFILE_LAYER];
      for (auto y : IndexRange(lut_size)) {
        for (auto x : IndexRange(lut_size)) {
          /* Repeatedly stored on every row for correct interpolation. */
          layer.data[y][x][0] = lut::burley_sss_profile[x][0];
          layer.data[y][x][1] = lut::random_walk_sss_profile[x][0];
          layer.data[y][x][2] = 0.0f;
          layer.data[y][x][UTIL_DISK_INTEGRAL_COMP] = lut::ltc_disk_integral[y][x][0];
        }
      }
      BLI_assert(UTIL_SSS_TRANSMITTANCE_PROFILE_LAYER == UTIL_DISK_INTEGRAL_LAYER);
    }
    {
      Layer &layer = data[UTIL_LTC_MAT_LAYER];
      memcpy(layer.data, lut::ltc_mat_ggx, sizeof(layer));
    }
    {
      Layer &layer = data[UTIL_BSDF_LAYER];
      for (auto x : IndexRange(lut_size)) {
        for (auto y : IndexRange(lut_size)) {
          layer.data[y][x][0] = lut::brdf_ggx[y][x][0];
          layer.data[y][x][1] = lut::brdf_ggx[y][x][1];
          layer.data[y][x][2] = lut::brdf_ggx[y][x][2];
          layer.data[y][x][3] = 0.0f;
        }
      }
    }
    {
      for (auto layer_id : IndexRange(16)) {
        Layer &layer = data[UTIL_BTDF_LAYER + layer_id];
        for (auto x : IndexRange(lut_size)) {
          for (auto y : IndexRange(lut_size)) {
            layer.data[y][x][0] = lut::bsdf_ggx[layer_id][y][x][0];
            layer.data[y][x][1] = lut::bsdf_ggx[layer_id][y][x][1];
            layer.data[y][x][2] = lut::bsdf_ggx[layer_id][y][x][2];
            layer.data[y][x][3] = lut::btdf_ggx[layer_id][y][x][0];
          }
        }
      }
    }
    GPU_texture_update_mipmap(*this, 0, GPU_DATA_FLOAT, data.data());
  }

  ~UtilityTexture(){};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pipelines
 *
 * Contains Shading passes. Shared between views. Objects will subscribe to at least one of them.
 * \{ */

class PipelineModule {
 public:
  BackgroundPipeline background;
  WorldPipeline world;
  WorldVolumePipeline world_volume;
  DeferredProbePipeline probe;
  DeferredPipeline deferred;
  ForwardPipeline forward;
  ShadowPipeline shadow;
  VolumePipeline volume;
  CapturePipeline capture;

  UtilityTexture utility_tx;

 public:
  PipelineModule(Instance &inst)
      : background(inst),
        world(inst),
        world_volume(inst),
        probe(inst),
        deferred(inst),
        forward(inst),
        shadow(inst),
        volume(inst),
        capture(inst){};

  void begin_sync()
  {
    probe.begin_sync();
    deferred.begin_sync();
    forward.sync();
    shadow.sync();
    volume.sync();
    capture.sync();
  }

  void end_sync()
  {
    probe.end_sync();
    deferred.end_sync();
  }

  PassMain::Sub *material_add(Object *ob,
                              ::Material *blender_mat,
                              GPUMaterial *gpumat,
                              eMaterialPipeline pipeline_type,
                              bool probe_capture)
  {
    if (probe_capture) {
      switch (pipeline_type) {
        case MAT_PIPE_DEFERRED_PREPASS:
          return probe.prepass_add(blender_mat, gpumat);
        case MAT_PIPE_DEFERRED:
          return probe.material_add(blender_mat, gpumat);
        default:
          break;
      }
    }

    switch (pipeline_type) {
      case MAT_PIPE_DEFERRED_PREPASS:
        return deferred.prepass_add(blender_mat, gpumat, false);
      case MAT_PIPE_FORWARD_PREPASS:
        if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
          return forward.prepass_transparent_add(ob, blender_mat, gpumat);
        }
        return forward.prepass_opaque_add(blender_mat, gpumat, false);

      case MAT_PIPE_DEFERRED_PREPASS_VELOCITY:
        return deferred.prepass_add(blender_mat, gpumat, true);
      case MAT_PIPE_FORWARD_PREPASS_VELOCITY:
        if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
          return forward.prepass_transparent_add(ob, blender_mat, gpumat);
        }
        return forward.prepass_opaque_add(blender_mat, gpumat, true);

      case MAT_PIPE_DEFERRED:
        return deferred.material_add(blender_mat, gpumat);
      case MAT_PIPE_FORWARD:
        if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
          return forward.material_transparent_add(ob, blender_mat, gpumat);
        }
        return forward.material_opaque_add(blender_mat, gpumat);
      case MAT_PIPE_VOLUME:
        return volume.volume_material_add(gpumat);
      case MAT_PIPE_SHADOW:
        return shadow.surface_material_add(gpumat);
      case MAT_PIPE_CAPTURE:
        return capture.surface_material_add(gpumat);
    }
    return nullptr;
  }
};

/** \} */

}  // namespace blender::eevee
