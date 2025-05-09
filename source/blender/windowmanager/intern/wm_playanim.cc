/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Animation player for image sequences & video's with sound support.
 * Launched in a separate process from Blender's #RENDER_OT_play_rendered_anim
 *
 * \note This file uses ghost directly and none of the WM definitions.
 * this could be made into its own module, alongside creator.
 */

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>

#ifndef WIN32
#  include <sys/times.h>
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  include <io.h>
#endif
#include "MEM_guardedalloc.h"

#include "PIL_time.h"

#include "CLG_log.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_path_util.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_system.h"
#include "BLI_utildefines.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "BKE_image.h"

#include "BIF_glutil.hh"

#include "GPU_context.h"
#include "GPU_framebuffer.h"
#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_init_exit.h"
#include "GPU_matrix.h"
#include "GPU_state.h"

#include "BLF_api.h"
#include "DNA_scene_types.h"
#include "ED_datafiles.h" /* for fonts */
#include "GHOST_C-api.h"

#include "DEG_depsgraph.hh"

#include "wm_window_private.h"

#include "WM_api.hh" /* only for WM_main_playanim */

#ifdef WITH_AUDASPACE
#  include <AUD_Device.h>
#  include <AUD_Handle.h>
#  include <AUD_Sound.h>
#  include <AUD_Special.h>

static AUD_Sound *source = nullptr;
static AUD_Handle *playback_handle = nullptr;
static AUD_Handle *scrub_handle = nullptr;
static AUD_Device *audio_device = nullptr;
#endif

/* simple limiter to avoid flooding memory */
#define USE_FRAME_CACHE_LIMIT
#ifdef USE_FRAME_CACHE_LIMIT
#  define PLAY_FRAME_CACHE_MAX 30
#endif

static CLG_LogRef LOG = {"wm.playanim"};

struct PlayState;
static void playanim_window_zoom(PlayState *ps, const float zoom_offset);
static bool playanim_window_font_scale_from_dpi(PlayState *ps);

/* -------------------------------------------------------------------- */
/** \name Local Utilities
 * \{ */

/**
 * \param filepath: The file path to read into memory.
 * \param r_mem: Optional, when nullptr, don't allocate memory (just set the size).
 * \param r_size: The file-size of `filepath`.
 */
static bool buffer_from_filepath(const char *filepath, void **r_mem, size_t *r_size)
{
  errno = 0;
  const int file = BLI_open(filepath, O_BINARY | O_RDONLY, 0);
  if (UNLIKELY(file == -1)) {
    CLOG_WARN(&LOG, "failure '%s' to open file '%s'", strerror(errno), filepath);
    return false;
  }

  bool success = false;
  uchar *mem = nullptr;
  const size_t size = BLI_file_descriptor_size(file);
  int64_t size_read;
  if (UNLIKELY(size == size_t(-1))) {
    CLOG_WARN(&LOG, "failure '%s' to access size '%s'", strerror(errno), filepath);
  }
  else if (r_mem && UNLIKELY(!(mem = static_cast<uchar *>(MEM_mallocN(size, __func__))))) {
    CLOG_WARN(&LOG, "error allocating buffer for '%s'", filepath);
  }
  else if (r_mem && UNLIKELY((size_read = BLI_read(file, mem, size)) != size)) {
    CLOG_WARN(&LOG,
              "error '%s' while reading '%s' (expected %" PRIu64 ", was %" PRId64 ")",
              strerror(errno),
              filepath,
              size,
              size_read);
  }
  else {
    close(file);
    *r_size = size;
    if (r_mem) {
      *r_mem = mem;
      mem = nullptr; /* `r_mem` owns, don't free on exit. */
    }
    success = true;
  }

  MEM_SAFE_FREE(mem);
  close(file);
  return success;
}

/** \} */

/** Use a flag to store held modifiers & mouse buttons. */
enum eWS_Qual {
  WS_QUAL_LSHIFT = (1 << 0),
  WS_QUAL_RSHIFT = (1 << 1),
#define WS_QUAL_SHIFT (WS_QUAL_LSHIFT | WS_QUAL_RSHIFT)
  WS_QUAL_LALT = (1 << 2),
  WS_QUAL_RALT = (1 << 3),
#define WS_QUAL_ALT (WS_QUAL_LALT | WS_QUAL_RALT)
  WS_QUAL_LCTRL = (1 << 4),
  WS_QUAL_RCTRL = (1 << 5),
#define WS_QUAL_CTRL (WS_QUAL_LCTRL | WS_QUAL_RCTRL)
  WS_QUAL_LMOUSE = (1 << 16),
  WS_QUAL_MMOUSE = (1 << 17),
  WS_QUAL_RMOUSE = (1 << 18),
#define WS_QUAL_MOUSE (WS_QUAL_LMOUSE | WS_QUAL_MMOUSE | WS_QUAL_RMOUSE)
};
ENUM_OPERATORS(eWS_Qual, WS_QUAL_RMOUSE)

struct GhostData {
  GHOST_SystemHandle system;
  GHOST_WindowHandle window;

  /** Not GHOST, but low level GPU context. */
  GPUContext *gpu_context;

  /** Held keys. */
  eWS_Qual qual;
};

/**
 * The minimal context necessary for displaying an image.
 * Used while displaying images both on load and while playing.
 */
struct PlayDisplayContext {
  ColorManagedViewSettings view_settings;
  ColorManagedDisplaySettings display_settings;
  /** Scale calculated from the DPI. */
  float ui_scale;
  /** Window & viewport size in pixels. */
  int size[2];
};

/**
 * The current state of the player.
 *
 * \warning Don't store results of parsing command-line arguments
 * in this struct if they need to persist across playing back different
 * files as these will be cleared when playing other files (drag & drop).
 */
struct PlayState {
  /** Context for displaying images (color spaces & display-size). */
  PlayDisplayContext display_ctx;

  /** Current zoom level. */
  float zoom;

  /** Playback direction (-1, 1). */
  short direction;
  /** Set the next frame to implement frame stepping (using shortcuts). */
  short next_frame;

  /** Playback once then wait. */
  bool once;
  /** Play forwards/backwards. */
  bool pingpong;
  /** Disable frame skipping. */
  bool noskip;
  /** Display current frame over the window. */
  bool indicator;
  /** Single-frame stepping has been enabled (frame loading and update pending). */
  bool sstep;
  /** Playback has stopped the image has been displayed. */
  bool wait2;
  /** Playback stopped state once stop/start variables have been handled. */
  bool stopped;
  /**
   * When disabled the current animation will exit,
   * after this either the application exits or a new animation window is opened.
   *
   * This is used so drag & drop can load new files which setup a newly created animation window.
   */
  bool go;
  /** True when waiting for images to load. */
  bool loading;
  /** X/Y image flip (set via key bindings). */
  bool draw_flip[2];

  /** The number of frames to step each update (default to 1, command line argument). */
  int fstep;

  /** Current frame (picture). */
  struct PlayAnimPict *picture;

  /** Image size in pixels, set once at the start. */
  int ibufx, ibufy;
  /** Mono-space font ID. */
  int fontid;
  int font_size;

  /** Restarts player for file drop (drag & drop). */
  char dropped_file[FILE_MAX];

  /** Force update when scrubbing with the cursor. */
  bool need_frame_update;
  /** The current frame calculated by scrubbing the mouse cursor. */
  int frame_cursor_x;

  GhostData ghost_data;
};

/* for debugging */
#if 0
static void print_ps(PlayState *ps)
{
  printf("ps:\n");
  printf("    direction=%d,\n", int(ps->direction));
  printf("    once=%d,\n", ps->once);
  printf("    pingpong=%d,\n", ps->pingpong);
  printf("    noskip=%d,\n", ps->noskip);
  printf("    sstep=%d,\n", ps->sstep);
  printf("    wait2=%d,\n", ps->wait2);
  printf("    stopped=%d,\n", ps->stopped);
  printf("    go=%d,\n\n", ps->go);
  fflush(stdout);
}
#endif

static void playanim_window_get_size(GHOST_WindowHandle ghost_window, int *r_width, int *r_height)
{
  GHOST_RectangleHandle bounds = GHOST_GetClientBounds(ghost_window);
  *r_width = GHOST_GetWidthRectangle(bounds);
  *r_height = GHOST_GetHeightRectangle(bounds);
  GHOST_DisposeRectangle(bounds);
}

static void playanim_gl_matrix()
{
  /* unified matrix, note it affects offset for drawing */
  /* NOTE: cannot use GPU_matrix_ortho_2d_set here because shader ignores. */
  GPU_matrix_ortho_set(0.0f, 1.0f, 0.0f, 1.0f, -1.0, 1.0f);
}

/* implementation */
static void playanim_event_qual_update(GhostData *ghost_data)
{
  bool val;

  /* Shift */
  GHOST_GetModifierKeyState(ghost_data->system, GHOST_kModifierKeyLeftShift, &val);
  SET_FLAG_FROM_TEST(ghost_data->qual, val, WS_QUAL_LSHIFT);

  GHOST_GetModifierKeyState(ghost_data->system, GHOST_kModifierKeyRightShift, &val);
  SET_FLAG_FROM_TEST(ghost_data->qual, val, WS_QUAL_RSHIFT);

  /* Control */
  GHOST_GetModifierKeyState(ghost_data->system, GHOST_kModifierKeyLeftControl, &val);
  SET_FLAG_FROM_TEST(ghost_data->qual, val, WS_QUAL_LCTRL);

  GHOST_GetModifierKeyState(ghost_data->system, GHOST_kModifierKeyRightControl, &val);
  SET_FLAG_FROM_TEST(ghost_data->qual, val, WS_QUAL_RCTRL);

  /* Alt */
  GHOST_GetModifierKeyState(ghost_data->system, GHOST_kModifierKeyLeftAlt, &val);
  SET_FLAG_FROM_TEST(ghost_data->qual, val, WS_QUAL_LALT);

  GHOST_GetModifierKeyState(ghost_data->system, GHOST_kModifierKeyRightAlt, &val);
  SET_FLAG_FROM_TEST(ghost_data->qual, val, WS_QUAL_RALT);
}

struct PlayAnimPict {
  PlayAnimPict *next, *prev;
  uchar *mem;
  size_t size;
  /** The allocated file-path to the image. */
  const char *filepath;
  ImBuf *ibuf;
  struct anim *anim;
  int frame;
  int IB_flags;

#ifdef USE_FRAME_CACHE_LIMIT
  /** Back pointer to the #LinkData node for this struct in the #g_frame_cache.pics list. */
  LinkData *frame_cache_node;
  size_t size_in_memory;
#endif
};

static ListBase picsbase = {nullptr, nullptr};
/* frames in memory - store them here to for easy deallocation later */
static bool fromdisk = false;
static double ptottime = 0.0, swaptime = 0.04;
#ifdef WITH_AUDASPACE
static double fps_movie;
#endif

#ifdef USE_FRAME_CACHE_LIMIT
static struct {
  /** A list of #LinkData nodes referencing #PlayAnimPict to track cached frames. */
  ListBase pics;
  /** Number if elements in `pics`. */
  int pics_len;
  /** Keep track of memory used by #g_frame_cache.pics when `g_frame_cache.memory_limit != 0`. */
  size_t pics_size_in_memory;
  /** Optionally limit the amount of memory used for cache (in bytes), ignored when zero. */
  size_t memory_limit;
} g_frame_cache = {
    /*pics*/ {nullptr, nullptr},
    /*pics_len*/ 0,
    /*pics_size_in_memory*/ 0,
    /*memory_limit*/ 0,
};

static void frame_cache_add(PlayAnimPict *pic)
{
  pic->frame_cache_node = BLI_genericNodeN(pic);
  BLI_addhead(&g_frame_cache.pics, pic->frame_cache_node);
  g_frame_cache.pics_len++;

  if (g_frame_cache.memory_limit != 0) {
    BLI_assert(pic->size_in_memory == 0);
    pic->size_in_memory = IMB_get_size_in_memory(pic->ibuf);
    g_frame_cache.pics_size_in_memory += pic->size_in_memory;
  }
}

static void frame_cache_remove(PlayAnimPict *pic)
{
  LinkData *node = pic->frame_cache_node;
  IMB_freeImBuf(pic->ibuf);
  if (g_frame_cache.memory_limit != 0) {
    BLI_assert(pic->size_in_memory != 0);
    g_frame_cache.pics_size_in_memory -= pic->size_in_memory;
    pic->size_in_memory = 0;
  }
  pic->ibuf = nullptr;
  pic->frame_cache_node = nullptr;
  BLI_freelinkN(&g_frame_cache.pics, node);
  g_frame_cache.pics_len--;
}

/* Don't free the current frame by moving it to the head of the list. */
static void frame_cache_touch(PlayAnimPict *pic)
{
  BLI_assert(pic->frame_cache_node->data == pic);
  BLI_remlink(&g_frame_cache.pics, pic->frame_cache_node);
  BLI_addhead(&g_frame_cache.pics, pic->frame_cache_node);
}

static bool frame_cache_limit_exceeded()
{
  return g_frame_cache.memory_limit ?
             (g_frame_cache.pics_size_in_memory > g_frame_cache.memory_limit) :
             (g_frame_cache.pics_len > PLAY_FRAME_CACHE_MAX);
}

static void frame_cache_limit_apply(ImBuf *ibuf_keep)
{
  /* Really basic memory conservation scheme. Keep frames in a FIFO queue. */
  LinkData *node = static_cast<LinkData *>(g_frame_cache.pics.last);
  while (node && frame_cache_limit_exceeded()) {
    PlayAnimPict *pic = static_cast<PlayAnimPict *>(node->data);
    BLI_assert(pic->frame_cache_node == node);

    node = node->prev;
    if (pic->ibuf && pic->ibuf != ibuf_keep) {
      frame_cache_remove(pic);
    }
  }
}

#endif /* USE_FRAME_CACHE_LIMIT */

static ImBuf *ibuf_from_picture(PlayAnimPict *pic)
{
  ImBuf *ibuf = nullptr;

  if (pic->ibuf) {
    ibuf = pic->ibuf;
  }
  else if (pic->anim) {
    ibuf = IMB_anim_absolute(pic->anim, pic->frame, IMB_TC_NONE, IMB_PROXY_NONE);
  }
  else if (pic->mem) {
    /* use correct colorspace here */
    ibuf = IMB_ibImageFromMemory(pic->mem, pic->size, pic->IB_flags, nullptr, pic->filepath);
  }
  else {
    /* use correct colorspace here */
    ibuf = IMB_loadiffname(pic->filepath, pic->IB_flags, nullptr);
  }

  return ibuf;
}

static PlayAnimPict *playanim_step(PlayAnimPict *playanim, int step)
{
  if (step > 0) {
    while (step-- && playanim) {
      playanim = playanim->next;
    }
  }
  else if (step < 0) {
    while (step++ && playanim) {
      playanim = playanim->prev;
    }
  }
  return playanim;
}

static int pupdate_time()
{
  static double time_last;

  double time = PIL_check_seconds_timer();

  ptottime += (time - time_last);
  time_last = time;
  return (ptottime < 0);
}

static void *ocio_transform_ibuf(const PlayDisplayContext *display_ctx,
                                 ImBuf *ibuf,
                                 bool *r_glsl_used,
                                 eGPUTextureFormat *r_format,
                                 eGPUDataFormat *r_data,
                                 void **r_buffer_cache_handle)
{
  void *display_buffer;
  bool force_fallback = false;
  *r_glsl_used = false;
  force_fallback |= (ED_draw_imbuf_method(ibuf) != IMAGE_DRAW_METHOD_GLSL);
  force_fallback |= (ibuf->dither != 0.0f);

  /* Default */
  *r_format = GPU_RGBA8;
  *r_data = GPU_DATA_UBYTE;

  /* Fallback to CPU based color space conversion. */
  if (force_fallback) {
    *r_glsl_used = false;
    display_buffer = nullptr;
  }
  else if (ibuf->float_buffer.data) {
    display_buffer = ibuf->float_buffer.data;

    *r_data = GPU_DATA_FLOAT;
    if (ibuf->channels == 4) {
      *r_format = GPU_RGBA16F;
    }
    else if (ibuf->channels == 3) {
      /* Alpha is implicitly 1. */
      *r_format = GPU_RGB16F;
    }

    if (ibuf->float_buffer.colorspace) {
      *r_glsl_used = IMB_colormanagement_setup_glsl_draw_from_space(&display_ctx->view_settings,
                                                                    &display_ctx->display_settings,
                                                                    ibuf->float_buffer.colorspace,
                                                                    ibuf->dither,
                                                                    false,
                                                                    false);
    }
    else {
      *r_glsl_used = IMB_colormanagement_setup_glsl_draw(
          &display_ctx->view_settings, &display_ctx->display_settings, ibuf->dither, false);
    }
  }
  else if (ibuf->byte_buffer.data) {
    display_buffer = ibuf->byte_buffer.data;
    *r_glsl_used = IMB_colormanagement_setup_glsl_draw_from_space(&display_ctx->view_settings,
                                                                  &display_ctx->display_settings,
                                                                  ibuf->byte_buffer.colorspace,
                                                                  ibuf->dither,
                                                                  false,
                                                                  false);
  }
  else {
    display_buffer = nullptr;
  }

  /* There is data to be displayed, but GLSL is not initialized
   * properly, in this case we fallback to CPU-based display transform. */
  if ((ibuf->byte_buffer.data || ibuf->float_buffer.data) && !*r_glsl_used) {
    display_buffer = IMB_display_buffer_acquire(
        ibuf, &display_ctx->view_settings, &display_ctx->display_settings, r_buffer_cache_handle);
    *r_format = GPU_RGBA8;
    *r_data = GPU_DATA_UBYTE;
  }

  return display_buffer;
}

static void draw_display_buffer(const PlayDisplayContext *display_ctx,
                                ImBuf *ibuf,
                                const rctf *canvas,
                                const bool draw_flip[2])
{
  /* Format needs to be created prior to any #immBindShader call.
   * Do it here because OCIO binds its own shader. */
  eGPUTextureFormat format;
  eGPUDataFormat data;
  bool glsl_used = false;
  GPUVertFormat *imm_format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(imm_format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  uint texCoord = GPU_vertformat_attr_add(
      imm_format, "texCoord", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

  void *buffer_cache_handle = nullptr;
  void *display_buffer = ocio_transform_ibuf(
      display_ctx, ibuf, &glsl_used, &format, &data, &buffer_cache_handle);

  /* NOTE: This may fail, especially for large images that exceed the GPU's texture size limit.
   * Large images could be supported although this isn't so common for animation playback. */
  GPUTexture *texture = GPU_texture_create_2d(
      "display_buf", ibuf->x, ibuf->y, 1, format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);

  if (texture) {
    GPU_texture_update(texture, data, display_buffer);
    GPU_texture_filter_mode(texture, false);

    GPU_texture_bind(texture, 0);
  }

  if (!glsl_used) {
    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
    immUniformColor3f(1.0f, 1.0f, 1.0f);
  }

  immBegin(GPU_PRIM_TRI_FAN, 4);

  rctf preview;
  BLI_rctf_init(&preview, 0.0f, 1.0f, 0.0f, 1.0f);
  if (draw_flip) {
    if (draw_flip[0]) {
      SWAP(float, preview.xmin, preview.xmax);
    }
    if (draw_flip[1]) {
      SWAP(float, preview.ymin, preview.ymax);
    }
  }

  immAttr2f(texCoord, preview.xmin, preview.ymin);
  immVertex2f(pos, canvas->xmin, canvas->ymin);

  immAttr2f(texCoord, preview.xmin, preview.ymax);
  immVertex2f(pos, canvas->xmin, canvas->ymax);

  immAttr2f(texCoord, preview.xmax, preview.ymax);
  immVertex2f(pos, canvas->xmax, canvas->ymax);

  immAttr2f(texCoord, preview.xmax, preview.ymin);
  immVertex2f(pos, canvas->xmax, canvas->ymin);

  immEnd();

  if (texture) {
    GPU_texture_unbind(texture);
    GPU_texture_free(texture);
  }

  if (!glsl_used) {
    immUnbindProgram();
  }
  else {
    IMB_colormanagement_finish_glsl_draw();
  }

  if (buffer_cache_handle) {
    IMB_display_buffer_release(buffer_cache_handle);
  }
}

/**
 * \param fontid: ID of the font to display (-1 when no text should be displayed).
 * \param fstep: Frame step (may be used in text display).
 * \param draw_zoom: Default to 1.0 (no zoom).
 * \param draw_flip: X/Y flipping (ignored when null).
 * \param indicator_factor: Display a vertical indicator (ignored when -1).
 */
static void playanim_toscreen_ex(GhostData *data,
                                 const PlayDisplayContext *display_ctx,
                                 const PlayAnimPict *picture,
                                 ImBuf *ibuf,
                                 /* Run-time drawing arguments (not used on-load). */
                                 const int fontid,
                                 const int fstep,
                                 const float draw_zoom,
                                 const bool draw_flip[2],
                                 const float indicator_factor)
{
  GHOST_ActivateWindowDrawingContext(data->window);
  GPU_render_begin();

  GPUContext *restore_context = GPU_context_active_get();
  GPU_context_active_set(data->gpu_context);

  GPU_clear_color(0.1f, 0.1f, 0.1f, 0.0f);

  /* A null `ibuf` is an exceptional case and should almost never happen.
   * if it does, this function displays a warning along with the file-path that failed. */
  if (ibuf) {
    /* Size within window. */
    float span_x = (draw_zoom * ibuf->x) / float(display_ctx->size[0]);
    float span_y = (draw_zoom * ibuf->y) / float(display_ctx->size[1]);

    /* offset within window */
    float offs_x = 0.5f * (1.0f - span_x);
    float offs_y = 0.5f * (1.0f - span_y);

    CLAMP(offs_x, 0.0f, 1.0f);
    CLAMP(offs_y, 0.0f, 1.0f);

    /* checkerboard for case alpha */
    if (ibuf->planes == 32) {
      GPU_blend(GPU_BLEND_ALPHA);

      imm_draw_box_checker_2d_ex(offs_x,
                                 offs_y,
                                 offs_x + span_x,
                                 offs_y + span_y,
                                 blender::float4{0.15, 0.15, 0.15, 1.0},
                                 blender::float4{0.20, 0.20, 0.20, 1.0},
                                 8);
    }
    rctf canvas;
    BLI_rctf_init(&canvas, offs_x, offs_x + span_x, offs_y, offs_y + span_y);

    draw_display_buffer(display_ctx, ibuf, &canvas, draw_flip);

    GPU_blend(GPU_BLEND_NONE);
  }

  pupdate_time();

  if ((fontid != -1) && picture) {
    const int font_margin = int(10 * display_ctx->ui_scale);
    int sizex, sizey;
    float fsizex_inv, fsizey_inv;
    char label[32 + FILE_MAX];
    if (ibuf) {
      SNPRINTF(label, "%s | %.2f frames/s", picture->filepath, fstep / swaptime);
    }
    else {
      SNPRINTF(label, "%s | <failed to load buffer>", picture->filepath);
    }

    playanim_window_get_size(data->window, &sizex, &sizey);
    fsizex_inv = 1.0f / sizex;
    fsizey_inv = 1.0f / sizey;

    BLF_color4f(fontid, 1.0, 1.0, 1.0, 1.0);

    /* FIXME(@ideasman42): Font positioning doesn't work because the aspect causes the position
     * to be rounded to zero, investigate making BLF support this,
     * for now use GPU matrix API to adjust the text position. */
#if 0
    BLF_enable(fontid, BLF_ASPECT);
    BLF_aspect(fontid, fsizex_inv, fsizey_inv, 1.0f);
    BLF_position(fontid, font_margin * fsizex_inv, font_margin * fsizey_inv, 0.0f);
    BLF_draw(fontid, label, sizeof(label));
#else
    GPU_matrix_push();
    GPU_matrix_scale_2f(fsizex_inv, fsizey_inv);
    GPU_matrix_translate_2f(font_margin, font_margin);
    BLF_position(fontid, 0, 0, 0.0f);
    BLF_draw(fontid, label, sizeof(label));
    GPU_matrix_pop();
#endif
  }

  if (indicator_factor != -1.0f) {
    float fac = indicator_factor;
    fac = 2.0f * fac - 1.0f;
    GPU_matrix_push_projection();
    GPU_matrix_identity_projection_set();
    GPU_matrix_push();
    GPU_matrix_identity_set();

    uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor3ub(0, 255, 0);

    immBegin(GPU_PRIM_LINES, 2);
    immVertex2f(pos, fac, -1.0f);
    immVertex2f(pos, fac, 1.0f);
    immEnd();

    immUnbindProgram();

    GPU_matrix_pop();
    GPU_matrix_pop_projection();
  }

  GPU_render_step();
  if (GPU_backend_get_type() == GPU_BACKEND_METAL) {
    GPU_flush();
  }

  GHOST_SwapWindowBuffers(data->window);
  GPU_context_active_set(restore_context);
  GPU_render_end();
}

static void playanim_toscreen_on_load(GhostData *ghost_data,
                                      const PlayDisplayContext *display_ctx,
                                      const PlayAnimPict *picture,
                                      ImBuf *ibuf)
{
  const int font_id = -1; /* Don't draw text. */
  const int fstep = -1;
  const float zoom = 1.0f;
  const float indicator_factor = -1.0f;
  const bool *draw_flip = nullptr;

  playanim_toscreen_ex(
      ghost_data, display_ctx, picture, ibuf, font_id, fstep, zoom, draw_flip, indicator_factor);
}

static void playanim_toscreen(PlayState *ps, const PlayAnimPict *picture, ImBuf *ibuf)
{
  float indicator_factor = -1.0f;
  if (ps->indicator) {
    const int frame_range = static_cast<const PlayAnimPict *>(picsbase.last)->frame -
                            static_cast<const PlayAnimPict *>(picsbase.first)->frame;
    if (frame_range > 0) {
      indicator_factor = float(double(picture->frame) / double(frame_range));
    }
    else {
      BLI_assert_msg(BLI_listbase_is_single(&picsbase), "Multiple frames without a valid range!");
    }
  }

  int fontid = -1;
  if ((ps->ghost_data.qual & (WS_QUAL_SHIFT | WS_QUAL_LMOUSE)) ||
      /* Always inform the user of an error, this should be an exceptional case. */
      (ibuf == nullptr))
  {
    fontid = ps->fontid;
  }

  BLI_assert(ps->loading == false);
  playanim_toscreen_ex(&ps->ghost_data,
                       &ps->display_ctx,
                       picture,
                       ibuf,
                       fontid,
                       ps->fstep,
                       ps->zoom,
                       ps->draw_flip,
                       indicator_factor);
}

static void build_pict_list_from_anim(GhostData *ghost_data,
                                      const PlayDisplayContext *display_ctx,
                                      const char *filepath_first,
                                      const int frame_offset)
{
  /* OCIO_TODO: support different input color space */
  anim *anim = IMB_open_anim(filepath_first, IB_rect, 0, nullptr);
  if (anim == nullptr) {
    CLOG_WARN(&LOG, "couldn't open anim '%s'", filepath_first);
    return;
  }

  ImBuf *ibuf = IMB_anim_absolute(anim, 0, IMB_TC_NONE, IMB_PROXY_NONE);
  if (ibuf) {
    playanim_toscreen_on_load(ghost_data, display_ctx, nullptr, ibuf);
    IMB_freeImBuf(ibuf);
  }

  for (int pic = 0; pic < IMB_anim_get_duration(anim, IMB_TC_NONE); pic++) {
    PlayAnimPict *picture = (PlayAnimPict *)MEM_callocN(sizeof(PlayAnimPict), "Pict");
    picture->anim = anim;
    picture->frame = pic + frame_offset;
    picture->IB_flags = IB_rect;
    picture->filepath = BLI_sprintfN("%s : %4.d", filepath_first, pic + 1);
    BLI_addtail(&picsbase, picture);
  }

  const PlayAnimPict *picture = (const PlayAnimPict *)picsbase.last;
  if (!(picture && picture->anim == anim)) {
    IMB_close_anim(anim);
    CLOG_WARN(&LOG, "no frames added for: '%s'", filepath_first);
  }
}

static void build_pict_list_from_image_sequence(GhostData *ghost_data,
                                                const PlayDisplayContext *display_ctx,
                                                const char *filepath_first,
                                                const int frame_offset,
                                                const int totframes,
                                                const int fstep,
                                                const bool *loading_p)
{
/* Load images into cache until the cache is full,
 * this resolves choppiness for images that are slow to load, see: #81751. */
#ifdef USE_FRAME_CACHE_LIMIT
  bool fill_cache = true;
#else
  bool fill_cache = false;
#endif

  int fp_framenr;
  struct {
    char head[FILE_MAX], tail[FILE_MAX];
    ushort digits;
  } fp_decoded;

  char filepath[FILE_MAX];
  STRNCPY(filepath, filepath_first);
  fp_framenr = BLI_path_sequence_decode(filepath,
                                        fp_decoded.head,
                                        sizeof(fp_decoded.head),
                                        fp_decoded.tail,
                                        sizeof(fp_decoded.tail),
                                        &fp_decoded.digits);

  pupdate_time();
  ptottime = 1.0;

  for (int pic = 0; pic < totframes; pic++) {
    if (!IMB_ispic(filepath)) {
      break;
    }

    void *mem = nullptr;
    size_t size = -1;
    if (!buffer_from_filepath(filepath, fromdisk ? nullptr : &mem, &size)) {
      /* A warning will have been logged. */
      break;
    }

    PlayAnimPict *picture = (PlayAnimPict *)MEM_callocN(sizeof(PlayAnimPict), "picture");
    picture->size = size;
    picture->IB_flags = IB_rect;
    picture->mem = static_cast<uchar *>(mem);
    picture->filepath = BLI_strdup(filepath);
    picture->frame = pic + frame_offset;
    BLI_addtail(&picsbase, picture);

    pupdate_time();

    const bool display_imbuf = ptottime > 1.0;

    if (display_imbuf || fill_cache) {
      /* OCIO_TODO: support different input color space */
      ImBuf *ibuf = ibuf_from_picture(picture);

      if (ibuf) {
        if (display_imbuf) {
          playanim_toscreen_on_load(ghost_data, display_ctx, picture, ibuf);
        }
#ifdef USE_FRAME_CACHE_LIMIT
        if (fill_cache) {
          picture->ibuf = ibuf;
          frame_cache_add(picture);
          fill_cache = !frame_cache_limit_exceeded();
        }
        else
#endif
        {
          IMB_freeImBuf(ibuf);
        }
      }

      if (display_imbuf) {
        pupdate_time();
        ptottime = 0.0;
      }
    }

    /* Create a new file-path each time. */
    fp_framenr += fstep;
    BLI_path_sequence_encode(filepath,
                             sizeof(filepath),
                             fp_decoded.head,
                             fp_decoded.tail,
                             fp_decoded.digits,
                             fp_framenr);

    while (GHOST_ProcessEvents(ghost_data->system, false)) {
      GHOST_DispatchEvents(ghost_data->system);
      if (*loading_p == false) {
        break;
      }
    }
  }
}

static void build_pict_list(GhostData *ghost_data,
                            const PlayDisplayContext *display_ctx,
                            const char *filepath_first,
                            const int totframes,
                            const int fstep,
                            bool *loading_p)
{
  *loading_p = true;

  /* NOTE(@ideasman42): When loading many files (expanded from shell globing for e.g.)
   * it's important the frame number increases each time. Otherwise playing `*.png`
   * in a directory will expand into many arguments, each calling this function adding
   * a frame that's set to zero. */
  const PlayAnimPict *picture_last = (PlayAnimPict *)picsbase.last;
  const int frame_offset = picture_last ? (picture_last->frame + 1) : 0;

  bool do_image_load = false;
  if (IMB_isanim(filepath_first)) {
    build_pict_list_from_anim(ghost_data, display_ctx, filepath_first, frame_offset);

    if (picsbase.last == picture_last) {
      /* FFMPEG detected JPEG2000 as a video which would load with zero duration.
       * Resolve this by using images as a fallback when a video file has no frames to display. */
      do_image_load = true;
    }
  }
  else {
    do_image_load = true;
  }

  if (do_image_load) {
    build_pict_list_from_image_sequence(
        ghost_data, display_ctx, filepath_first, frame_offset, totframes, fstep, loading_p);
  }

  *loading_p = false;
}

static void update_sound_fps()
{
#ifdef WITH_AUDASPACE
  if (playback_handle) {
    /* swaptime stores the 1.0/fps ratio */
    double speed = 1.0 / (swaptime * fps_movie);

    AUD_Handle_setPitch(playback_handle, speed);
  }
#endif
}

static void tag_change_frame(PlayState *ps, int cx)
{
  ps->need_frame_update = true;
  ps->frame_cursor_x = cx;
}

static void change_frame(PlayState *ps)
{
  if (!ps->need_frame_update) {
    return;
  }

  int sizex, sizey;
  int i, i_last;

  if (BLI_listbase_is_empty(&picsbase)) {
    return;
  }

  playanim_window_get_size(ps->ghost_data.window, &sizex, &sizey);
  i_last = ((PlayAnimPict *)picsbase.last)->frame;
  i = (i_last * ps->frame_cursor_x) / sizex;
  CLAMP(i, 0, i_last);

#ifdef WITH_AUDASPACE
  if (scrub_handle) {
    AUD_Handle_stop(scrub_handle);
    scrub_handle = nullptr;
  }

  if (playback_handle) {
    AUD_Status status = AUD_Handle_getStatus(playback_handle);
    if (status != AUD_STATUS_PLAYING) {
      AUD_Handle_stop(playback_handle);
      playback_handle = AUD_Device_play(audio_device, source, 1);
      if (playback_handle) {
        AUD_Handle_setPosition(playback_handle, i / fps_movie);
        scrub_handle = AUD_pauseAfter(playback_handle, 1 / fps_movie);
      }
      update_sound_fps();
    }
    else {
      AUD_Handle_setPosition(playback_handle, i / fps_movie);
      scrub_handle = AUD_pauseAfter(playback_handle, 1 / fps_movie);
    }
  }
  else if (source) {
    playback_handle = AUD_Device_play(audio_device, source, 1);
    if (playback_handle) {
      AUD_Handle_setPosition(playback_handle, i / fps_movie);
      scrub_handle = AUD_pauseAfter(playback_handle, 1 / fps_movie);
    }
    update_sound_fps();
  }
#endif

  ps->picture = static_cast<PlayAnimPict *>(BLI_findlink(&picsbase, i));
  BLI_assert(ps->picture != nullptr);

  ps->sstep = true;
  ps->wait2 = false;
  ps->next_frame = 0;

  ps->need_frame_update = false;
}

static void playanim_audio_resume(PlayState *ps)
{
#ifdef WITH_AUDASPACE
  /* TODO: store in ps direct? */
  const int i = BLI_findindex(&picsbase, ps->picture);
  if (playback_handle) {
    AUD_Handle_stop(playback_handle);
  }
  playback_handle = AUD_Device_play(audio_device, source, 1);
  if (playback_handle) {
    AUD_Handle_setPosition(playback_handle, i / fps_movie);
  }
  update_sound_fps();
#else
  UNUSED_VARS(ps);
#endif
}

static void playanim_audio_stop(PlayState * /*ps*/)
{
#ifdef WITH_AUDASPACE
  if (playback_handle) {
    AUD_Handle_stop(playback_handle);
    playback_handle = nullptr;
  }
#endif
}

static bool ghost_event_proc(GHOST_EventHandle evt, GHOST_TUserDataPtr ps_void)
{
  PlayState *ps = (PlayState *)ps_void;
  const GHOST_TEventType type = GHOST_GetEventType(evt);
  /* Convert ghost event into value keyboard or mouse. */
  const int val = ELEM(type, GHOST_kEventKeyDown, GHOST_kEventButtonDown);
  GHOST_SystemHandle ghost_system = ps->ghost_data.system;
  GHOST_WindowHandle ghost_window = ps->ghost_data.window;

  // print_ps(ps);

  playanim_event_qual_update(&ps->ghost_data);

  /* first check if we're busy loading files */
  if (ps->loading) {
    switch (type) {
      case GHOST_kEventKeyDown:
      case GHOST_kEventKeyUp: {
        GHOST_TEventKeyData *key_data;

        key_data = (GHOST_TEventKeyData *)GHOST_GetEventData(evt);
        switch (key_data->key) {
          case GHOST_kKeyEsc:
            ps->loading = false;
            break;
          default:
            break;
        }
        break;
      }
      default:
        break;
    }
    return true;
  }

  if (ps->wait2 && ps->stopped == false) {
    ps->stopped = true;
  }

  if (ps->wait2) {
    pupdate_time();
    ptottime = 0;
  }

  switch (type) {
    case GHOST_kEventKeyDown:
    case GHOST_kEventKeyUp: {
      GHOST_TEventKeyData *key_data;

      key_data = (GHOST_TEventKeyData *)GHOST_GetEventData(evt);
      switch (key_data->key) {
        case GHOST_kKeyA:
          if (val) {
            ps->noskip = !ps->noskip;
          }
          break;
        case GHOST_kKeyI:
          if (val) {
            ps->indicator = !ps->indicator;
          }
          break;
        case GHOST_kKeyP:
          if (val) {
            ps->pingpong = !ps->pingpong;
          }
          break;
        case GHOST_kKeyF: {
          if (val) {
            int axis = (ps->ghost_data.qual & WS_QUAL_SHIFT) ? 1 : 0;
            ps->draw_flip[axis] = !ps->draw_flip[axis];
          }
          break;
        }
        case GHOST_kKey1:
        case GHOST_kKeyNumpad1:
          if (val) {
            swaptime = ps->fstep / 60.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey2:
        case GHOST_kKeyNumpad2:
          if (val) {
            swaptime = ps->fstep / 50.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey3:
        case GHOST_kKeyNumpad3:
          if (val) {
            swaptime = ps->fstep / 30.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey4:
        case GHOST_kKeyNumpad4:
          if (ps->ghost_data.qual & WS_QUAL_SHIFT) {
            swaptime = ps->fstep / 24.0;
            update_sound_fps();
          }
          else {
            swaptime = ps->fstep / 25.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey5:
        case GHOST_kKeyNumpad5:
          if (val) {
            swaptime = ps->fstep / 20.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey6:
        case GHOST_kKeyNumpad6:
          if (val) {
            swaptime = ps->fstep / 15.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey7:
        case GHOST_kKeyNumpad7:
          if (val) {
            swaptime = ps->fstep / 12.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey8:
        case GHOST_kKeyNumpad8:
          if (val) {
            swaptime = ps->fstep / 10.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKey9:
        case GHOST_kKeyNumpad9:
          if (val) {
            swaptime = ps->fstep / 6.0;
            update_sound_fps();
          }
          break;
        case GHOST_kKeyLeftArrow:
          if (val) {
            ps->sstep = true;
            ps->wait2 = false;
            playanim_audio_stop(ps);

            if (ps->ghost_data.qual & WS_QUAL_SHIFT) {
              ps->picture = static_cast<PlayAnimPict *>(picsbase.first);
              ps->next_frame = 0;
            }
            else {
              ps->next_frame = -1;
            }
          }
          break;
        case GHOST_kKeyDownArrow:
          if (val) {
            ps->wait2 = false;
            playanim_audio_stop(ps);

            if (ps->ghost_data.qual & WS_QUAL_SHIFT) {
              ps->next_frame = ps->direction = -1;
            }
            else {
              ps->next_frame = -10;
              ps->sstep = true;
            }
          }
          break;
        case GHOST_kKeyRightArrow:
          if (val) {
            ps->sstep = true;
            ps->wait2 = false;
            playanim_audio_stop(ps);

            if (ps->ghost_data.qual & WS_QUAL_SHIFT) {
              ps->picture = static_cast<PlayAnimPict *>(picsbase.last);
              ps->next_frame = 0;
            }
            else {
              ps->next_frame = 1;
            }
          }
          break;
        case GHOST_kKeyUpArrow:
          if (val) {
            ps->wait2 = false;

            if (ps->ghost_data.qual & WS_QUAL_SHIFT) {
              ps->next_frame = ps->direction = 1;
              if (ps->sstep == false) {
                playanim_audio_resume(ps);
              }
            }
            else {
              ps->next_frame = 10;
              ps->sstep = true;
              playanim_audio_stop(ps);
            }
          }
          break;

        case GHOST_kKeySlash:
        case GHOST_kKeyNumpadSlash:
          if (val) {
            if (ps->ghost_data.qual & WS_QUAL_SHIFT) {
              if (ps->picture && ps->picture->ibuf) {
                printf(" Name: %s | Speed: %.2f frames/s\n",
                       ps->picture->ibuf->filepath,
                       ps->fstep / swaptime);
              }
            }
            else {
              swaptime = ps->fstep / 5.0;
              update_sound_fps();
            }
          }
          break;
        case GHOST_kKey0:
        case GHOST_kKeyNumpad0:
          if (val) {
            if (ps->once) {
              ps->once = ps->wait2 = false;
            }
            else {
              ps->picture = nullptr;
              ps->once = true;
              ps->wait2 = false;
            }
          }
          break;

        case GHOST_kKeySpace:
          if (val) {
            if (ps->wait2 || ps->sstep) {
              ps->wait2 = ps->sstep = false;
              playanim_audio_resume(ps);
            }
            else {
              ps->sstep = true;
              ps->wait2 = true;
              playanim_audio_stop(ps);
            }
          }
          break;
        case GHOST_kKeyEnter:
        case GHOST_kKeyNumpadEnter:
          if (val) {
            ps->wait2 = ps->sstep = false;
            playanim_audio_resume(ps);
          }
          break;
        case GHOST_kKeyPeriod:
        case GHOST_kKeyNumpadPeriod:
          if (val) {
            if (ps->sstep) {
              ps->wait2 = false;
            }
            else {
              ps->sstep = true;
              ps->wait2 = !ps->wait2;
              playanim_audio_stop(ps);
            }
          }
          break;
        case GHOST_kKeyEqual:
        case GHOST_kKeyPlus:
        case GHOST_kKeyNumpadPlus: {
          if (val == 0) {
            break;
          }
          if (ps->ghost_data.qual & WS_QUAL_CTRL) {
            playanim_window_zoom(ps, 0.1f);
          }
          else {
            if (swaptime > ps->fstep / 60.0) {
              swaptime /= 1.1;
              update_sound_fps();
            }
          }
          break;
        }
        case GHOST_kKeyMinus:
        case GHOST_kKeyNumpadMinus: {
          if (val == 0) {
            break;
          }
          if (ps->ghost_data.qual & WS_QUAL_CTRL) {
            playanim_window_zoom(ps, -0.1f);
          }
          else {
            if (swaptime < ps->fstep / 5.0) {
              swaptime *= 1.1;
              update_sound_fps();
            }
          }
          break;
        }
        case GHOST_kKeyEsc:
          ps->go = false;
          break;
        default:
          break;
      }
      break;
    }
    case GHOST_kEventButtonDown:
    case GHOST_kEventButtonUp: {
      GHOST_TEventButtonData *bd = reinterpret_cast<GHOST_TEventButtonData *>(
          GHOST_GetEventData(evt));
      int cx, cy, sizex, sizey;
      playanim_window_get_size(ghost_window, &sizex, &sizey);

      const bool inside_window = (GHOST_GetCursorPosition(ghost_system, ghost_window, &cx, &cy) ==
                                  GHOST_kSuccess) &&
                                 (cx >= 0 && cx < sizex && cy >= 0 && cy <= sizey);

      if (bd->button == GHOST_kButtonMaskLeft) {
        if (type == GHOST_kEventButtonDown) {
          if (inside_window) {
            ps->ghost_data.qual |= WS_QUAL_LMOUSE;
            tag_change_frame(ps, cx);
          }
        }
        else {
          ps->ghost_data.qual &= ~WS_QUAL_LMOUSE;
        }
      }
      else if (bd->button == GHOST_kButtonMaskMiddle) {
        if (type == GHOST_kEventButtonDown) {
          if (inside_window) {
            ps->ghost_data.qual |= WS_QUAL_MMOUSE;
          }
        }
        else {
          ps->ghost_data.qual &= ~WS_QUAL_MMOUSE;
        }
      }
      else if (bd->button == GHOST_kButtonMaskRight) {
        if (type == GHOST_kEventButtonDown) {
          if (inside_window) {
            ps->ghost_data.qual |= WS_QUAL_RMOUSE;
          }
        }
        else {
          ps->ghost_data.qual &= ~WS_QUAL_RMOUSE;
        }
      }
      break;
    }
    case GHOST_kEventCursorMove: {
      if (ps->ghost_data.qual & WS_QUAL_LMOUSE) {
        GHOST_TEventCursorData *cd = reinterpret_cast<GHOST_TEventCursorData *>(
            GHOST_GetEventData(evt));
        int cx, cy;

        /* Ignore 'in-between' events, since they can make scrubbing lag.
         *
         * Ideally we would keep into the event queue and see if this is the last motion event.
         * however the API currently doesn't support this. */
        {
          int x_test, y_test;
          if (GHOST_GetCursorPosition(ghost_system, ghost_window, &cx, &cy) == GHOST_kSuccess) {
            GHOST_ScreenToClient(ghost_window, cd->x, cd->y, &x_test, &y_test);
            if (cx != x_test || cy != y_test) {
              /* we're not the last event... skipping */
              break;
            }
          }
        }

        tag_change_frame(ps, cx);
      }
      break;
    }
    case GHOST_kEventWindowActivate:
    case GHOST_kEventWindowDeactivate: {
      ps->ghost_data.qual &= ~WS_QUAL_MOUSE;
      break;
    }
    case GHOST_kEventWindowSize:
    case GHOST_kEventWindowMove: {
      float zoomx, zoomy;

      playanim_window_get_size(ghost_window, &ps->display_ctx.size[0], &ps->display_ctx.size[1]);
      GHOST_ActivateWindowDrawingContext(ghost_window);

      zoomx = float(ps->display_ctx.size[0]) / ps->ibufx;
      zoomy = float(ps->display_ctx.size[1]) / ps->ibufy;

      /* zoom always show entire image */
      ps->zoom = MIN2(zoomx, zoomy);

      GPU_viewport(0, 0, ps->display_ctx.size[0], ps->display_ctx.size[1]);
      GPU_scissor(0, 0, ps->display_ctx.size[0], ps->display_ctx.size[1]);

      playanim_gl_matrix();

      ptottime = 0.0;

      playanim_toscreen(ps, ps->picture, ps->picture ? ps->picture->ibuf : nullptr);

      break;
    }
    case GHOST_kEventQuitRequest:
    case GHOST_kEventWindowClose: {
      ps->go = false;
      break;
    }
    case GHOST_kEventWindowDPIHintChanged: {
      /* Rely on frame-change to redraw. */
      playanim_window_font_scale_from_dpi(ps);
      break;
    }
    case GHOST_kEventDraggingDropDone: {
      GHOST_TEventDragnDropData *ddd = reinterpret_cast<GHOST_TEventDragnDropData *>(
          GHOST_GetEventData(evt));

      if (ddd->dataType == GHOST_kDragnDropTypeFilenames) {
        GHOST_TStringArray *stra = reinterpret_cast<GHOST_TStringArray *>(ddd->data);
        int a;

        for (a = 0; a < stra->count; a++) {
          STRNCPY(ps->dropped_file, (char *)stra->strings[a]);
          ps->go = false;
          printf("drop file %s\n", stra->strings[a]);
          break; /* only one drop element supported now */
        }
      }
      break;
    }
    default:
      /* quiet warnings */
      break;
  }

  return true;
}

static GHOST_WindowHandle playanim_window_open(
    GHOST_SystemHandle ghost_system, const char *title, int posx, int posy, int sizex, int sizey)
{
  GHOST_GPUSettings gpusettings = {0};
  const eGPUBackendType gpu_backend = GPU_backend_type_selection_get();
  gpusettings.context_type = wm_ghost_drawing_context_type(gpu_backend);

  {
    bool screen_size_valid = false;
    uint32_t screen_size[2];
    if ((GHOST_GetMainDisplayDimensions(ghost_system, &screen_size[0], &screen_size[1]) ==
         GHOST_kSuccess) &&
        (screen_size[0] > 0) && (screen_size[1] > 0))
    {
      screen_size_valid = true;
    }
    else {
      /* Unlikely the screen size fails to access,
       * if this happens it's still important to clamp the window size by *something*. */
      screen_size[0] = 1024;
      screen_size[1] = 1024;
    }

    if (screen_size_valid) {
      if (GHOST_GetCapabilities() & GHOST_kCapabilityWindowPosition) {
        posy = (screen_size[1] - posy - sizey);
      }
    }
    else {
      posx = 0;
      posy = 0;
    }

    /* NOTE: ideally the GPU could be queried for the maximum supported window size,
     * this isn't so simple as the GPU back-end's capabilities are initialized *after* the window
     * has been created. Further, it's quite unlikely the users main monitor size is larger
     * than the maximum window size supported by the GPU. */

    /* Clamp the size so very large requests aren't rejected by the GPU.
     * Halve until a usable range is reached instead of scaling down to meet the screen size
     * since fractional scaling tends not to look so nice. */
    while (sizex >= int(screen_size[0]) || sizey >= int(screen_size[1])) {
      sizex /= 2;
      sizey /= 2;
    }
    /* Unlikely but ensure the size is *never* zero. */
    CLAMP_MIN(sizex, 1);
    CLAMP_MIN(sizey, 1);
  }

  return GHOST_CreateWindow(ghost_system,
                            nullptr,
                            title,
                            posx,
                            posy,
                            sizex,
                            sizey,
                            /* Could optionally start full-screen. */
                            GHOST_kWindowStateNormal,
                            false,
                            gpusettings);
}

static void playanim_window_zoom(PlayState *ps, const float zoom_offset)
{
  int sizex, sizey;
  // int ofsx, ofsy; /* UNUSED */

  if (ps->zoom + zoom_offset > 0.0f) {
    ps->zoom += zoom_offset;
  }

  // playanim_window_get_position(&ofsx, &ofsy);
  playanim_window_get_size(ps->ghost_data.window, &sizex, &sizey);
  // ofsx += sizex / 2; /* UNUSED */
  // ofsy += sizey / 2; /* UNUSED */
  sizex = ps->zoom * ps->ibufx;
  sizey = ps->zoom * ps->ibufy;
  // ofsx -= sizex / 2; /* UNUSED */
  // ofsy -= sizey / 2; /* UNUSED */
  // window_set_position(ps->ghost_data.window, sizex, sizey);
  GHOST_SetClientSize(ps->ghost_data.window, sizex, sizey);
}

static bool playanim_window_font_scale_from_dpi(PlayState *ps)
{
  const float scale = (GHOST_GetDPIHint(ps->ghost_data.window) / 96.0f);
  const float font_size_base = 11.0f; /* Font size un-scaled. */
  const int font_size = int(font_size_base * scale) + 0.5f;
  bool changed = false;
  if (ps->font_size != font_size) {
    BLF_size(ps->fontid, font_size);
    ps->font_size = font_size;
    changed = true;
  }
  if (ps->display_ctx.ui_scale != scale) {
    ps->display_ctx.ui_scale = scale;
  }
  return changed;
}

/**
 * \return The a path used to restart the animation player or nullptr to exit.
 */
static char *wm_main_playanim_intern(int argc, const char **argv)
{
  ImBuf *ibuf = nullptr;
  static char filepath[FILE_MAX]; /* abused to return dropped file path */
  int i;
  /* This was done to disambiguate the name for use under c++. */
  int start_x = 0, start_y = 0;
  int sfra = -1;
  int efra = -1;
  int totblock;

  PlayState ps{};

  ps.go = true;
  ps.direction = true;
  ps.next_frame = 1;
  ps.once = false;
  ps.pingpong = false;
  ps.noskip = false;
  ps.sstep = false;
  ps.wait2 = false;
  ps.stopped = false;
  ps.loading = false;
  ps.picture = nullptr;
  ps.indicator = false;
  ps.dropped_file[0] = 0;
  ps.zoom = 1.0f;
  ps.draw_flip[0] = false;
  ps.draw_flip[1] = false;

  ps.fstep = 1;

  ps.fontid = -1;

  STRNCPY(ps.display_ctx.display_settings.display_device,
          IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DEFAULT_BYTE));
  IMB_colormanagement_init_default_view_settings(&ps.display_ctx.view_settings,
                                                 &ps.display_ctx.display_settings);
  ps.display_ctx.ui_scale = 1.0f;

  /* Skip the first argument which is assumed to be '-a' (used to launch this player). */
  while (argc > 1) {
    if (argv[1][0] == '-') {
      switch (argv[1][1]) {
        case 'm':
          fromdisk = true;
          break;
        case 'p':
          if (argc > 3) {
            start_x = atoi(argv[2]);
            start_y = atoi(argv[3]);
            argc -= 2;
            argv += 2;
          }
          else {
            printf("too few arguments for -p (need 2): skipping\n");
          }
          break;
        case 'f':
          if (argc > 3) {
            double fps = atof(argv[2]);
            double fps_base = atof(argv[3]);
            if (fps == 0.0) {
              fps = 1;
              printf(
                  "invalid fps,"
                  "forcing 1\n");
            }
            swaptime = fps_base / fps;
            argc -= 2;
            argv += 2;
          }
          else {
            printf("too few arguments for -f (need 2): skipping\n");
          }
          break;
        case 's':
          sfra = atoi(argv[2]);
          CLAMP(sfra, 1, MAXFRAME);
          argc--;
          argv++;
          break;
        case 'e':
          efra = atoi(argv[2]);
          CLAMP(efra, 1, MAXFRAME);
          argc--;
          argv++;
          break;
        case 'j':
          ps.fstep = atoi(argv[2]);
          CLAMP(ps.fstep, 1, MAXFRAME);
          swaptime *= ps.fstep;
          argc--;
          argv++;
          break;
        case 'c': {
#ifdef USE_FRAME_CACHE_LIMIT
          const int memory_in_mb = max_ii(0, atoi(argv[2]));
          g_frame_cache.memory_limit = size_t(memory_in_mb) * (1024 * 1024);
#endif
          argc--;
          argv++;
          break;
        }
        default:
          printf("unknown option '%c': skipping\n", argv[1][1]);
          break;
      }
      argc--;
      argv++;
    }
    else {
      break;
    }
  }

  if (argc > 1) {
    STRNCPY(filepath, argv[1]);
  }
  else {
    printf("%s: no filepath argument given\n", __func__);
    exit(EXIT_FAILURE);
  }

  if (IMB_isanim(filepath)) {
    /* OCIO_TODO: support different input color spaces */
    anim *anim;
    anim = IMB_open_anim(filepath, IB_rect, 0, nullptr);
    if (anim) {
      ibuf = IMB_anim_absolute(anim, 0, IMB_TC_NONE, IMB_PROXY_NONE);
      IMB_close_anim(anim);
      anim = nullptr;
    }
  }
  else if (!IMB_ispic(filepath)) {
    printf("%s: '%s' not an image file\n", __func__, filepath);
    exit(EXIT_FAILURE);
  }

  if (ibuf == nullptr) {
    /* OCIO_TODO: support different input color space */
    ibuf = IMB_loadiffname(filepath, IB_rect, nullptr);
  }

  if (ibuf == nullptr) {
    printf("%s: '%s' couldn't open\n", __func__, filepath);
    exit(EXIT_FAILURE);
  }

  /* Select GPU backend. */
  GPU_backend_type_selection_detect();

  /* Init GHOST and open window. */
  GHOST_EventConsumerHandle ghost_event_consumer = nullptr;
  {
    ghost_event_consumer = GHOST_CreateEventConsumer(ghost_event_proc, &ps);

    GHOST_SetBacktraceHandler((GHOST_TBacktraceFn)BLI_system_backtrace);

    ps.ghost_data.system = GHOST_CreateSystem();

    if (UNLIKELY(ps.ghost_data.system == nullptr)) {
      /* GHOST will have reported the back-ends that failed to load. */
      CLOG_WARN(&LOG, "GHOST: unable to initialize, exiting!");
      /* This will leak memory, it's preferable to crashing. */
      exit(EXIT_FAILURE);
    }

    GHOST_AddEventConsumer(ps.ghost_data.system, ghost_event_consumer);

    ps.ghost_data.window = playanim_window_open(
        ps.ghost_data.system, "Blender Animation Player", start_x, start_y, ibuf->x, ibuf->y);
  }

  // GHOST_ActivateWindowDrawingContext(ps.ghost_data.window);

  /* Init Blender GPU context. */
  ps.ghost_data.gpu_context = GPU_context_create(ps.ghost_data.window, nullptr);
  GPU_init();

  /* initialize the font */
  BLF_init();
  ps.fontid = BLF_load_mono_default(false);

  ps.font_size = -1; /* Force update. */
  playanim_window_font_scale_from_dpi(&ps);

  ps.ibufx = ibuf->x;
  ps.ibufy = ibuf->y;

  ps.display_ctx.size[0] = ps.ibufx;
  ps.display_ctx.size[1] = ps.ibufy;

  GPU_render_begin();
  GPU_render_step();
  GPU_clear_color(0.1f, 0.1f, 0.1f, 0.0f);

  {
    int window_size[2];
    playanim_window_get_size(ps.ghost_data.window, &window_size[0], &window_size[1]);
    GPU_viewport(0, 0, window_size[0], window_size[1]);
    GPU_scissor(0, 0, window_size[0], window_size[1]);
    playanim_gl_matrix();
  }

  GHOST_SwapWindowBuffers(ps.ghost_data.window);
  GPU_render_end();

  if (sfra == -1 || efra == -1) {
    /* one of the frames was invalid, just use all images */
    sfra = 1;
    efra = MAXFRAME;
  }

  build_pict_list(
      &ps.ghost_data, &ps.display_ctx, filepath, (efra - sfra) + 1, ps.fstep, &ps.loading);

#ifdef WITH_AUDASPACE
  source = AUD_Sound_file(filepath);
  if (!BLI_listbase_is_empty(&picsbase)) {
    anim *anim_movie = ((PlayAnimPict *)picsbase.first)->anim;
    if (anim_movie) {
      short frs_sec = 25;
      float frs_sec_base = 1.0;

      IMB_anim_get_fps(anim_movie, &frs_sec, &frs_sec_base, true);

      fps_movie = double(frs_sec) / double(frs_sec_base);
      /* enforce same fps for movie as sound */
      swaptime = ps.fstep / fps_movie;
    }
  }
#endif

  for (i = 2; i < argc; i++) {
    STRNCPY(filepath, argv[i]);
    build_pict_list(
        &ps.ghost_data, &ps.display_ctx, filepath, (efra - sfra) + 1, ps.fstep, &ps.loading);
  }

  IMB_freeImBuf(ibuf);
  ibuf = nullptr;

  pupdate_time();
  ptottime = 0;

/* newly added in 2.6x, without this images never get freed */
#define USE_IMB_CACHE

  while (ps.go) {
    if (ps.pingpong) {
      ps.direction = -ps.direction;
    }

    if (ps.direction == 1) {
      ps.picture = static_cast<PlayAnimPict *>(picsbase.first);
    }
    else {
      ps.picture = static_cast<PlayAnimPict *>(picsbase.last);
    }

    if (ps.picture == nullptr) {
      printf("couldn't find pictures\n");
      ps.go = false;
    }
    if (ps.pingpong) {
      if (ps.direction == 1) {
        ps.picture = ps.picture->next;
      }
      else {
        ps.picture = ps.picture->prev;
      }
    }
    if (ptottime > 0.0) {
      ptottime = 0.0;
    }

#ifdef WITH_AUDASPACE
    if (playback_handle) {
      AUD_Handle_stop(playback_handle);
    }
    playback_handle = AUD_Device_play(audio_device, source, 1);
    update_sound_fps();
#endif

    while (ps.picture) {
      bool has_event;
#ifndef USE_IMB_CACHE
      if (ibuf != nullptr && ibuf->ftype == IMB_FTYPE_NONE) {
        IMB_freeImBuf(ibuf);
      }
#endif

      ibuf = ibuf_from_picture(ps.picture);

      {
#ifdef USE_IMB_CACHE
        ps.picture->ibuf = ibuf;
#endif
        if (ibuf) {
#ifdef USE_FRAME_CACHE_LIMIT
          if (ps.picture->frame_cache_node == nullptr) {
            frame_cache_add(ps.picture);
          }
          else {
            frame_cache_touch(ps.picture);
          }
          frame_cache_limit_apply(ibuf);
#endif /* USE_FRAME_CACHE_LIMIT */

          STRNCPY(ibuf->filepath, ps.picture->filepath);
        }

/* why only windows? (from 2.4x) - campbell */
#ifdef _WIN32
        GHOST_SetTitle(ps.ghost_data.window, ps.picture->filepath);
#endif

        while (pupdate_time()) {
          PIL_sleep_ms(1);
        }
        ptottime -= swaptime;
        playanim_toscreen(&ps, ps.picture, ibuf);
      }

      if (ps.once) {
        if (ps.picture->next == nullptr) {
          ps.wait2 = true;
        }
        else if (ps.picture->prev == nullptr) {
          ps.wait2 = true;
        }
      }

      ps.next_frame = ps.direction;

      GPU_render_begin();
      GPUContext *restore_context = GPU_context_active_get();
      GPU_context_active_set(ps.ghost_data.gpu_context);
      while ((has_event = GHOST_ProcessEvents(ps.ghost_data.system, false))) {
        GHOST_DispatchEvents(ps.ghost_data.system);
      }
      GPU_render_end();
      GPU_context_active_set(restore_context);

      if (ps.go == false) {
        break;
      }
      change_frame(&ps);
      if (!has_event) {
        PIL_sleep_ms(1);
      }
      if (ps.wait2) {
        continue;
      }

      ps.wait2 = ps.sstep;

      if (ps.wait2 == false && ps.stopped) {
        ps.stopped = false;
      }

      pupdate_time();

      if (ps.picture && ps.next_frame) {
        /* Advance to the next frame, always at least set one step.
         * Implement frame-skipping when enabled and playback is not fast enough. */
        while (ps.picture) {
          ps.picture = playanim_step(ps.picture, ps.next_frame);

          if (ps.once && ps.picture != nullptr) {
            if (ps.picture->next == nullptr) {
              ps.wait2 = true;
            }
            else if (ps.picture->prev == nullptr) {
              ps.wait2 = true;
            }
          }

          if (ps.wait2 || ptottime < swaptime || ps.noskip) {
            break;
          }
          ptottime -= swaptime;
        }
        if (ps.picture == nullptr && ps.sstep) {
          ps.picture = playanim_step(ps.picture, ps.next_frame);
        }
      }
      if (ps.go == false) {
        break;
      }
    }
  }
  while ((ps.picture = static_cast<PlayAnimPict *>(BLI_pophead(&picsbase)))) {
    if (ps.picture->anim) {
      if ((ps.picture->next == nullptr) || (ps.picture->next->anim != ps.picture->anim)) {
        IMB_close_anim(ps.picture->anim);
      }
    }

    if (ps.picture->ibuf) {
      IMB_freeImBuf(ps.picture->ibuf);
    }
    if (ps.picture->mem) {
      MEM_freeN(ps.picture->mem);
    }

    MEM_freeN((void *)ps.picture->filepath);
    MEM_freeN(ps.picture);
  }

/* cleanup */
#ifndef USE_IMB_CACHE
  if (ibuf) {
    IMB_freeImBuf(ibuf);
  }
#endif

  BLI_freelistN(&picsbase);

#ifdef USE_FRAME_CACHE_LIMIT
  BLI_freelistN(&g_frame_cache.pics);
  g_frame_cache.pics_len = 0;
  g_frame_cache.pics_size_in_memory = 0;
#endif

#ifdef WITH_AUDASPACE
  if (playback_handle) {
    AUD_Handle_stop(playback_handle);
    playback_handle = nullptr;
  }
  if (scrub_handle) {
    AUD_Handle_stop(scrub_handle);
    scrub_handle = nullptr;
  }
  AUD_Sound_free(source);
  source = nullptr;
#endif

  /* we still miss freeing a lot!,
   * but many areas could skip initialization too for anim play */

  DEG_free_node_types();

  BLF_exit();

  /* NOTE: Must happen before GPU Context destruction as GPU resources are released via
   * Color Management module. Must be re-initialized in the case of drag & drop. */
  IMB_exit();

  if (ps.ghost_data.gpu_context) {
    GPU_context_active_set(ps.ghost_data.gpu_context);
    GPU_exit();
    GPU_context_discard(ps.ghost_data.gpu_context);
    ps.ghost_data.gpu_context = nullptr;
  }
  GHOST_RemoveEventConsumer(ps.ghost_data.system, ghost_event_consumer);
  GHOST_DisposeEventConsumer(ghost_event_consumer);

  GHOST_DisposeWindow(ps.ghost_data.system, ps.ghost_data.window);

  /* early exit, IMB and BKE should be exited only in end */
  if (ps.dropped_file[0]) {
    /* Ensure drag & drop runs with a valid IMB state. */
    IMB_init();

    STRNCPY(filepath, ps.dropped_file);
    return filepath;
  }

  GHOST_DisposeSystem(ps.ghost_data.system);

  totblock = MEM_get_memory_blocks_in_use();
  if (totblock != 0) {
/* prints many bAKey, bArgument's which are tricky to fix */
#if 0
    printf("Error Totblock: %d\n", totblock);
    MEM_printmemlist();
#endif
  }

  return nullptr;
}

void WM_main_playanim(int argc, const char **argv)
{
  const char *argv_next[2];
  bool looping = true;

#ifdef WITH_AUDASPACE
  {
    AUD_DeviceSpecs specs;

    specs.rate = AUD_RATE_48000;
    specs.format = AUD_FORMAT_FLOAT32;
    specs.channels = AUD_CHANNELS_STEREO;

    AUD_initOnce();

    if (!(audio_device = AUD_init(nullptr, specs, 1024, "Blender"))) {
      audio_device = AUD_init("None", specs, 0, "Blender");
    }
  }
#endif

  while (looping) {
    const char *filepath = wm_main_playanim_intern(argc, argv);

    if (filepath) { /* use simple args */
      argv_next[0] = argv[0];
      argv_next[1] = filepath;
      argc = 2;

      /* continue with new args */
      argv = argv_next;
    }
    else {
      looping = false;
    }
  }

#ifdef WITH_AUDASPACE
  AUD_exit(audio_device);
  AUD_exitOnce();
#endif
}
