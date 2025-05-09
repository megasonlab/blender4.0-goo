/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Handle OpenGL buffers for windowing, also paint cursor.
 */

#include <cstdlib>
#include <cstring>

#include "DNA_camera_types.h"
#include "DNA_listBase.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_image.h"
#include "BKE_main.h"
#include "BKE_scene.h"
#include "BKE_screen.hh"

#include "GHOST_C-api.h"

#include "ED_node.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "GPU_batch_presets.h"
#include "GPU_capabilities.h"
#include "GPU_context.h"
#include "GPU_debug.h"
#include "GPU_framebuffer.h"
#include "GPU_immediate.h"
#include "GPU_matrix.h"
#include "GPU_state.h"
#include "GPU_texture.h"
#include "GPU_viewport.h"

#include "RE_engine.h"

#include "WM_api.hh"
#include "WM_toolsystem.h"
#include "WM_types.hh"
#include "wm.hh"
#include "wm_draw.hh"
#include "wm_event_system.h"
#include "wm_surface.hh"
#include "wm_window.hh"

#include "UI_resources.hh"

#ifdef WITH_OPENSUBDIV
#  include "BKE_subsurf.hh"
#endif

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

/**
 * Return true when the cursor is grabbed and wrapped within a region.
 */
static bool wm_window_grab_warp_region_is_set(const wmWindow *win)
{
  if (ELEM(win->grabcursor, GHOST_kGrabWrap, GHOST_kGrabHide)) {
    GHOST_TGrabCursorMode mode_dummy;
    GHOST_TAxisFlag wrap_axis_dummy;
    int bounds[4] = {0};
    bool use_software_cursor_dummy = false;
    GHOST_GetCursorGrabState(static_cast<GHOST_WindowHandle>(win->ghostwin),
                             &mode_dummy,
                             &wrap_axis_dummy,
                             bounds,
                             &use_software_cursor_dummy);
    if ((bounds[0] != bounds[2]) || (bounds[1] != bounds[3])) {
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw Paint Cursor
 * \{ */

static void wm_paintcursor_draw(bContext *C, ScrArea *area, ARegion *region)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  bScreen *screen = WM_window_get_active_screen(win);

  /* Don't draw paint cursors with locked interface. Painting is not possible
   * then, and cursor drawing can use scene data that another thread may be
   * modifying. */
  if (wm->is_interface_locked) {
    return;
  }

  if (!region->visible || region != screen->active_region) {
    return;
  }

  LISTBASE_FOREACH_MUTABLE (wmPaintCursor *, pc, &wm->paintcursors) {
    if ((pc->space_type != SPACE_TYPE_ANY) && (area->spacetype != pc->space_type)) {
      continue;
    }

    if (!ELEM(pc->region_type, RGN_TYPE_ANY, region->regiontype)) {
      continue;
    }

    if (pc->poll == nullptr || pc->poll(C)) {
      UI_SetTheme(area->spacetype, region->regiontype);

      /* Prevent drawing outside region. */
      GPU_scissor_test(true);
      GPU_scissor(region->winrct.xmin,
                  region->winrct.ymin,
                  BLI_rcti_size_x(&region->winrct) + 1,
                  BLI_rcti_size_y(&region->winrct) + 1);
      /* Reading the cursor location from the operating-system while the cursor is grabbed
       * conflicts with grabbing logic that hides the cursor, then keeps it centered to accumulate
       * deltas without it escaping from the window. In this case we never want to show the actual
       * cursor coordinates so limit reading the cursor location to when the cursor is grabbed and
       * wrapping in a region since this is the case when it would otherwise attempt to draw the
       * cursor outside the view/window. See: #102792. */
      const int *xy = win->eventstate->xy;
      int xy_buf[2];
      if ((WM_capabilities_flag() & WM_CAPABILITY_CURSOR_WARP) &&
          wm_window_grab_warp_region_is_set(win) &&
          wm_cursor_position_get(win, &xy_buf[0], &xy_buf[1]))
      {
        xy = xy_buf;
      }

      pc->draw(C, xy[0], xy[1], pc->customdata);
      GPU_scissor_test(false);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw Software Cursor
 *
 * Draw the cursor instead of relying on the graphical environment.
 * Needed when setting the cursor position (warping) isn't supported (GHOST/WAYLAND).
 * \{ */

/**
 * Track the state of the last drawn cursor.
 */
static struct {
  int8_t enabled;
  int winid;
  int xy[2];
} g_software_cursor = {
    /*enabled*/ -1,
    /*winid*/ -1,
};

/** Reuse the result from #GHOST_GetCursorGrabState. */
struct GrabState {
  GHOST_TGrabCursorMode mode;
  GHOST_TAxisFlag wrap_axis;
  int bounds[4];
};

static bool wm_software_cursor_needed()
{
  if (UNLIKELY(g_software_cursor.enabled == -1)) {
    g_software_cursor.enabled = !(WM_capabilities_flag() & WM_CAPABILITY_CURSOR_WARP);
  }
  return g_software_cursor.enabled;
}

static bool wm_software_cursor_needed_for_window(const wmWindow *win, GrabState *grab_state)
{
  BLI_assert(wm_software_cursor_needed());
  if (GHOST_GetCursorVisibility(static_cast<GHOST_WindowHandle>(win->ghostwin))) {
    /* NOTE: The value in `win->grabcursor` can't be used as it
     * doesn't always match GHOST's value in the case of tablet events. */
    bool use_software_cursor;
    GHOST_GetCursorGrabState(static_cast<GHOST_WindowHandle>(win->ghostwin),
                             &grab_state->mode,
                             &grab_state->wrap_axis,
                             grab_state->bounds,
                             &use_software_cursor);
    if (use_software_cursor) {
      return true;
    }
  }
  return false;
}

static bool wm_software_cursor_motion_test(const wmWindow *win)
{
  return (g_software_cursor.winid != win->winid) ||
         (g_software_cursor.xy[0] != win->eventstate->xy[0]) ||
         (g_software_cursor.xy[1] != win->eventstate->xy[1]);
}

static void wm_software_cursor_motion_update(const wmWindow *win)
{

  g_software_cursor.winid = win->winid;
  g_software_cursor.xy[0] = win->eventstate->xy[0];
  g_software_cursor.xy[1] = win->eventstate->xy[1];
}

static void wm_software_cursor_motion_clear()
{
  g_software_cursor.winid = -1;
  g_software_cursor.xy[0] = -1;
  g_software_cursor.xy[1] = -1;
}

static void wm_software_cursor_motion_clear_with_window(const wmWindow *win)
{
  if (g_software_cursor.winid == win->winid) {
    wm_software_cursor_motion_clear();
  }
}

static void wm_software_cursor_draw_bitmap(const int event_xy[2],
                                           const GHOST_CursorBitmapRef *bitmap)
{
  GPU_blend(GPU_BLEND_ALPHA);

  float gl_matrix[4][4];
  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL;
  GPUTexture *texture = GPU_texture_create_2d(
      "softeare_cursor", bitmap->data_size[0], bitmap->data_size[1], 1, GPU_RGBA8, usage, nullptr);
  GPU_texture_update(texture, GPU_DATA_UBYTE, bitmap->data);
  GPU_texture_filter_mode(texture, false);

  GPU_matrix_push();

  const int scale = int(U.pixelsize);

  unit_m4(gl_matrix);

  gl_matrix[3][0] = event_xy[0] - (bitmap->hot_spot[0] * scale);
  gl_matrix[3][1] = event_xy[1] - ((bitmap->data_size[1] - bitmap->hot_spot[1]) * scale);

  gl_matrix[0][0] = bitmap->data_size[0] * scale;
  gl_matrix[1][1] = bitmap->data_size[1] * scale;

  GPU_matrix_mul(gl_matrix);

  GPUVertFormat *imm_format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(imm_format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  uint texCoord = GPU_vertformat_attr_add(
      imm_format, "texCoord", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

  /* Use 3D image for correct display of planar tracked images. */
  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE);

  immBindTexture("image", texture);

  immBegin(GPU_PRIM_TRI_FAN, 4);

  immAttr2f(texCoord, 0.0f, 1.0f);
  immVertex3f(pos, 0.0f, 0.0f, 0.0f);

  immAttr2f(texCoord, 1.0f, 1.0f);
  immVertex3f(pos, 1.0f, 0.0f, 0.0f);

  immAttr2f(texCoord, 1.0f, 0.0f);
  immVertex3f(pos, 1.0f, 1.0f, 0.0f);

  immAttr2f(texCoord, 0.0f, 0.0f);
  immVertex3f(pos, 0.0f, 1.0f, 0.0f);

  immEnd();

  immUnbindProgram();

  GPU_matrix_pop();
  GPU_texture_unbind(texture);
  GPU_texture_free(texture);

  GPU_blend(GPU_BLEND_NONE);
}

static void wm_software_cursor_draw_crosshair(const int event_xy[2])
{
  /* Draw a primitive cross-hair cursor.
   * NOTE: the `win->cursor` could be used for drawing although it's complicated as some cursors
   * are set by the operating-system, where the pixel information isn't easily available. */
  const float unit = max_ff(UI_SCALE_FAC, 1.0f);
  uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", GPU_COMP_I32, 2, GPU_FETCH_INT_TO_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immUniformColor4f(1, 1, 1, 1);
  {
    const int ofs_line = (8 * unit);
    const int ofs_size = (2 * unit);
    immRecti(pos,
             event_xy[0] - ofs_line,
             event_xy[1] - ofs_size,
             event_xy[0] + ofs_line,
             event_xy[1] + ofs_size);
    immRecti(pos,
             event_xy[0] - ofs_size,
             event_xy[1] - ofs_line,
             event_xy[0] + ofs_size,
             event_xy[1] + ofs_line);
  }
  immUniformColor4f(0, 0, 0, 1);
  {
    const int ofs_line = (7 * unit);
    const int ofs_size = (1 * unit);
    immRecti(pos,
             event_xy[0] - ofs_line,
             event_xy[1] - ofs_size,
             event_xy[0] + ofs_line,
             event_xy[1] + ofs_size);
    immRecti(pos,
             event_xy[0] - ofs_size,
             event_xy[1] - ofs_line,
             event_xy[0] + ofs_size,
             event_xy[1] + ofs_line);
  }
  immUnbindProgram();
}

static void wm_software_cursor_draw(wmWindow *win, const GrabState *grab_state)
{
  int event_xy[2] = {UNPACK2(win->eventstate->xy)};

  if (grab_state->wrap_axis & GHOST_kAxisX) {
    const int min = grab_state->bounds[0];
    const int max = grab_state->bounds[2];
    if (min != max) {
      event_xy[0] = mod_i(event_xy[0] - min, max - min) + min;
    }
  }
  if (grab_state->wrap_axis & GHOST_kAxisY) {
    const int height = WM_window_pixels_y(win);
    const int min = height - grab_state->bounds[1];
    const int max = height - grab_state->bounds[3];
    if (min != max) {
      event_xy[1] = mod_i(event_xy[1] - max, min - max) + max;
    }
  }

  GHOST_CursorBitmapRef bitmap = {nullptr};
  if (GHOST_GetCursorBitmap(static_cast<GHOST_WindowHandle>(win->ghostwin), &bitmap) ==
      GHOST_kSuccess)
  {
    wm_software_cursor_draw_bitmap(event_xy, &bitmap);
  }
  else {
    wm_software_cursor_draw_crosshair(event_xy);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Post Draw Region on display handlers
 * \{ */

static void wm_region_draw_overlay(bContext *C, ScrArea *area, ARegion *region)
{
  wmWindow *win = CTX_wm_window(C);

  wmViewport(&region->winrct);
  UI_SetTheme(area->spacetype, region->regiontype);
  region->type->draw_overlay(C, region);
  wmWindowViewport(win);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

static bool wm_draw_region_stereo_set(Main *bmain,
                                      ScrArea *area,
                                      ARegion *region,
                                      eStereoViews sview)
{
  /* We could detect better when stereo is actually needed, by inspecting the
   * image in the image editor and sequencer. */
  if (!ELEM(region->regiontype, RGN_TYPE_WINDOW, RGN_TYPE_PREVIEW)) {
    return false;
  }

  switch (area->spacetype) {
    case SPACE_IMAGE: {
      if (region->regiontype == RGN_TYPE_WINDOW) {
        SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
        sima->iuser.multiview_eye = sview;
        return true;
      }
      break;
    }
    case SPACE_VIEW3D: {
      if (region->regiontype == RGN_TYPE_WINDOW) {
        View3D *v3d = static_cast<View3D *>(area->spacedata.first);
        if (v3d->camera && v3d->camera->type == OB_CAMERA) {
          RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
          RenderEngine *engine = rv3d->view_render ? RE_view_engine_get(rv3d->view_render) :
                                                     nullptr;
          if (engine && !(engine->type->flag & RE_USE_STEREO_VIEWPORT)) {
            return false;
          }

          Camera *cam = static_cast<Camera *>(v3d->camera->data);
          CameraBGImage *bgpic = static_cast<CameraBGImage *>(cam->bg_images.first);
          v3d->multiview_eye = sview;
          if (bgpic) {
            bgpic->iuser.multiview_eye = sview;
          }
          return true;
        }
      }
      break;
    }
    case SPACE_NODE: {
      if (region->regiontype == RGN_TYPE_WINDOW) {
        SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
        if ((snode->flag & SNODE_BACKDRAW) && ED_node_is_compositor(snode)) {
          Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Viewer Node");
          ima->eye = sview;
          return true;
        }
      }
      break;
    }
    case SPACE_SEQ: {
      SpaceSeq *sseq = static_cast<SpaceSeq *>(area->spacedata.first);
      sseq->multiview_eye = sview;

      if (region->regiontype == RGN_TYPE_PREVIEW) {
        return true;
      }
      if (region->regiontype == RGN_TYPE_WINDOW) {
        return (sseq->draw_flag & SEQ_DRAW_BACKDROP) != 0;
      }
    }
  }

  return false;
}

static void wm_region_test_gizmo_do_draw(bContext *C,
                                         ScrArea *area,
                                         ARegion *region,
                                         bool tag_redraw)
{
  if (region->gizmo_map == nullptr) {
    return;
  }

  wmGizmoMap *gzmap = region->gizmo_map;
  LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, WM_gizmomap_group_list(gzmap)) {
    if (tag_redraw && (gzgroup->type->flag & WM_GIZMOGROUPTYPE_VR_REDRAWS)) {
      ScrArea *ctx_area = CTX_wm_area(C);
      ARegion *ctx_region = CTX_wm_region(C);

      CTX_wm_area_set(C, area);
      CTX_wm_region_set(C, region);

      if (WM_gizmo_group_type_poll(C, gzgroup->type)) {
        ED_region_tag_redraw_editor_overlays(region);
      }

      /* Reset. */
      CTX_wm_area_set(C, ctx_area);
      CTX_wm_region_set(C, ctx_region);
    }

    LISTBASE_FOREACH (wmGizmo *, gz, &gzgroup->gizmos) {
      if (gz->do_draw) {
        if (tag_redraw) {
          ED_region_tag_redraw_editor_overlays(region);
        }
        gz->do_draw = false;
      }
    }
  }
}

static void wm_region_test_render_do_draw(const Scene *scene,
                                          Depsgraph *depsgraph,
                                          ScrArea *area,
                                          ARegion *region)
{
  /* tag region for redraw from render engine preview running inside of it */
  if (area->spacetype == SPACE_VIEW3D && region->regiontype == RGN_TYPE_WINDOW) {
    RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
    RenderEngine *engine = rv3d->view_render ? RE_view_engine_get(rv3d->view_render) : nullptr;
    GPUViewport *viewport = WM_draw_region_get_viewport(region);

    if (engine && (engine->flag & RE_ENGINE_DO_DRAW)) {
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      rcti border_rect;

      /* do partial redraw when possible */
      if (ED_view3d_calc_render_border(scene, depsgraph, v3d, region, &border_rect)) {
        ED_region_tag_redraw_partial(region, &border_rect, false);
      }
      else {
        ED_region_tag_redraw_no_rebuild(region);
      }

      engine->flag &= ~RE_ENGINE_DO_DRAW;
    }
    else if (viewport && GPU_viewport_do_update(viewport)) {
      ED_region_tag_redraw_no_rebuild(region);
    }
  }
}

#ifdef WITH_XR_OPENXR
static void wm_region_test_xr_do_draw(const wmWindowManager *wm,
                                      const ScrArea *area,
                                      ARegion *region)
{
  if ((area->spacetype == SPACE_VIEW3D) && (region->regiontype == RGN_TYPE_WINDOW)) {
    if (ED_view3d_is_region_xr_mirror_active(
            wm, static_cast<const View3D *>(area->spacedata.first), region))
    {
      ED_region_tag_redraw_no_rebuild(region);
    }
  }
}
#endif

static bool wm_region_use_viewport_by_type(short space_type, short region_type)
{
  return (ELEM(space_type, SPACE_VIEW3D, SPACE_IMAGE, SPACE_NODE) &&
          region_type == RGN_TYPE_WINDOW) ||
         ((space_type == SPACE_SEQ) && ELEM(region_type, RGN_TYPE_PREVIEW, RGN_TYPE_WINDOW));
}

bool WM_region_use_viewport(ScrArea *area, ARegion *region)
{
  return wm_region_use_viewport_by_type(area->spacetype, region->regiontype);
}

static const char *wm_area_name(ScrArea *area)
{
#define SPACE_NAME(space) \
  case space: \
    return #space;

  switch (area->spacetype) {
    SPACE_NAME(SPACE_EMPTY);
    SPACE_NAME(SPACE_VIEW3D);
    SPACE_NAME(SPACE_GRAPH);
    SPACE_NAME(SPACE_OUTLINER);
    SPACE_NAME(SPACE_PROPERTIES);
    SPACE_NAME(SPACE_FILE);
    SPACE_NAME(SPACE_IMAGE);
    SPACE_NAME(SPACE_INFO);
    SPACE_NAME(SPACE_SEQ);
    SPACE_NAME(SPACE_TEXT);
    SPACE_NAME(SPACE_ACTION);
    SPACE_NAME(SPACE_NLA);
    SPACE_NAME(SPACE_SCRIPT);
    SPACE_NAME(SPACE_NODE);
    SPACE_NAME(SPACE_CONSOLE);
    SPACE_NAME(SPACE_USERPREF);
    SPACE_NAME(SPACE_CLIP);
    SPACE_NAME(SPACE_TOPBAR);
    SPACE_NAME(SPACE_STATUSBAR);
    default:
      return "Unknown Space";
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Drawing (Draw All)
 *
 * Reference method, draw all each time.
 * \{ */

struct WindowDrawCB {
  WindowDrawCB *next, *prev;

  void (*draw)(const wmWindow *, void *);
  void *customdata;
};

void *WM_draw_cb_activate(wmWindow *win, void (*draw)(const wmWindow *, void *), void *customdata)
{
  WindowDrawCB *wdc = static_cast<WindowDrawCB *>(MEM_callocN(sizeof(*wdc), "WindowDrawCB"));

  BLI_addtail(&win->drawcalls, wdc);
  wdc->draw = draw;
  wdc->customdata = customdata;

  return wdc;
}

void WM_draw_cb_exit(wmWindow *win, void *handle)
{
  LISTBASE_FOREACH (WindowDrawCB *, wdc, &win->drawcalls) {
    if (wdc == (WindowDrawCB *)handle) {
      BLI_remlink(&win->drawcalls, wdc);
      MEM_freeN(wdc);
      return;
    }
  }
}

static void wm_draw_callbacks(wmWindow *win)
{
  LISTBASE_FOREACH (WindowDrawCB *, wdc, &win->drawcalls) {
    wdc->draw(win, wdc->customdata);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Drawing
 *
 * Each region draws into its own frame-buffer, which is then blit on the
 * window draw buffer. This helps with fast redrawing if only some regions
 * change. It also means we can share a single context for multiple windows,
 * so that for example VAOs can be shared between windows.
 * \{ */

static void wm_draw_region_buffer_free(ARegion *region)
{
  if (region->draw_buffer) {
    if (region->draw_buffer->viewport) {
      GPU_viewport_free(region->draw_buffer->viewport);
    }
    if (region->draw_buffer->offscreen) {
      GPU_offscreen_free(region->draw_buffer->offscreen);
    }

    MEM_freeN(region->draw_buffer);
    region->draw_buffer = nullptr;
  }
}

static void wm_draw_offscreen_texture_parameters(GPUOffScreen *offscreen)
{
  /* Setup offscreen color texture for drawing. */
  GPUTexture *texture = GPU_offscreen_color_texture(offscreen);

  /* No mipmaps or filtering. */
  GPU_texture_mipmap_mode(texture, false, false);
}

static eGPUTextureFormat get_hdr_framebuffer_format(const Scene *scene)
{
  bool use_hdr = false;
  if (scene && ((scene->view_settings.flag & COLORMANAGE_VIEW_USE_HDR) != 0)) {
    use_hdr = GPU_hdr_support();
  }
  eGPUTextureFormat desired_format = (use_hdr) ? GPU_RGBA16F : GPU_RGBA8;
  return desired_format;
}

static void wm_draw_region_buffer_create(Scene *scene,
                                         ARegion *region,
                                         bool stereo,
                                         bool use_viewport)
{

  /* Determine desired offscreen format depending on HDR availability. */
  eGPUTextureFormat desired_format = get_hdr_framebuffer_format(scene);

  if (region->draw_buffer) {
    if (region->draw_buffer->stereo != stereo) {
      /* Free draw buffer on stereo changes. */
      wm_draw_region_buffer_free(region);
    }
    else {
      /* Free offscreen buffer on size changes. Viewport auto resizes. */
      GPUOffScreen *offscreen = region->draw_buffer->offscreen;
      if (offscreen && (GPU_offscreen_width(offscreen) != region->winx ||
                        GPU_offscreen_height(offscreen) != region->winy ||
                        GPU_offscreen_format(offscreen) != desired_format))
      {
        wm_draw_region_buffer_free(region);
      }
    }
  }

  if (!region->draw_buffer) {
    if (use_viewport) {
      /* Allocate viewport which includes an off-screen buffer with depth multi-sample, etc. */
      region->draw_buffer = static_cast<wmDrawBuffer *>(
          MEM_callocN(sizeof(wmDrawBuffer), "wmDrawBuffer"));
      region->draw_buffer->viewport = stereo ? GPU_viewport_stereo_create() :
                                               GPU_viewport_create();
    }
    else {
      /* Allocate off-screen buffer if it does not exist. This one has no
       * depth or multi-sample buffers. 3D view creates own buffers with
       * the data it needs. */
      GPUOffScreen *offscreen = GPU_offscreen_create(region->winx,
                                                     region->winy,
                                                     false,
                                                     desired_format,
                                                     GPU_TEXTURE_USAGE_SHADER_READ,
                                                     nullptr);
      if (!offscreen) {
        WM_report(RPT_ERROR, "Region could not be drawn!");
        return;
      }

      wm_draw_offscreen_texture_parameters(offscreen);

      region->draw_buffer = static_cast<wmDrawBuffer *>(
          MEM_callocN(sizeof(wmDrawBuffer), "wmDrawBuffer"));
      region->draw_buffer->offscreen = offscreen;
    }

    region->draw_buffer->bound_view = -1;
    region->draw_buffer->stereo = stereo;
  }
}

static void wm_draw_region_bind(ARegion *region, int view)
{
  if (!region->draw_buffer) {
    return;
  }

  if (region->draw_buffer->viewport) {
    GPU_viewport_bind(region->draw_buffer->viewport, view, &region->winrct);
  }
  else {
    GPU_offscreen_bind(region->draw_buffer->offscreen, false);

    /* For now scissor is expected by region drawing, we could disable it
     * and do the enable/disable in the specific cases that setup scissor. */
    GPU_scissor_test(true);
    GPU_scissor(0, 0, region->winx, region->winy);
  }

  region->draw_buffer->bound_view = view;
}

static void wm_draw_region_unbind(ARegion *region)
{
  if (!region->draw_buffer) {
    return;
  }

  region->draw_buffer->bound_view = -1;

  if (region->draw_buffer->viewport) {
    GPU_viewport_unbind(region->draw_buffer->viewport);
  }
  else {
    GPU_scissor_test(false);
    GPU_offscreen_unbind(region->draw_buffer->offscreen, false);
  }
}

static void wm_draw_region_blit(ARegion *region, int view)
{
  if (!region->draw_buffer) {
    return;
  }

  if (view == -1) {
    /* Non-stereo drawing. */
    view = 0;
  }
  else if (view > 0) {
    if (region->draw_buffer->viewport == nullptr) {
      /* Region does not need stereo or failed to allocate stereo buffers. */
      view = 0;
    }
  }

  if (region->draw_buffer->viewport) {
    GPU_viewport_draw_to_screen(region->draw_buffer->viewport, view, &region->winrct);
  }
  else {
    GPU_offscreen_draw_to_screen(
        region->draw_buffer->offscreen, region->winrct.xmin, region->winrct.ymin);
  }
}

GPUTexture *wm_draw_region_texture(ARegion *region, int view)
{
  if (!region->draw_buffer) {
    return nullptr;
  }

  GPUViewport *viewport = region->draw_buffer->viewport;
  if (viewport) {
    return GPU_viewport_color_texture(viewport, view);
  }
  return GPU_offscreen_color_texture(region->draw_buffer->offscreen);
}

void wm_draw_region_blend(ARegion *region, int view, bool blend)
{
  if (!region->draw_buffer) {
    return;
  }

  /* Alpha is always 1, except when blend timer is running. */
  float alpha = ED_region_blend_alpha(region);
  if (alpha <= 0.0f) {
    return;
  }

  if (!blend) {
    alpha = 1.0f;
  }

  /* wmOrtho for the screen has this same offset */
  const float halfx = GLA_PIXEL_OFS / (BLI_rcti_size_x(&region->winrct) + 1);
  const float halfy = GLA_PIXEL_OFS / (BLI_rcti_size_y(&region->winrct) + 1);

  rcti rect_geo = region->winrct;
  rect_geo.xmax += 1;
  rect_geo.ymax += 1;

  rctf rect_tex;
  rect_tex.xmin = halfx;
  rect_tex.ymin = halfy;
  rect_tex.xmax = 1.0f + halfx;
  rect_tex.ymax = 1.0f + halfy;

  float alpha_easing = 1.0f - alpha;
  alpha_easing = 1.0f - alpha_easing * alpha_easing;

  /* Slide vertical panels */
  float ofs_x = BLI_rcti_size_x(&region->winrct) * (1.0f - alpha_easing);
  if (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) == RGN_ALIGN_RIGHT) {
    rect_geo.xmin += ofs_x;
    rect_tex.xmax *= alpha_easing;
    alpha = 1.0f;
  }
  else if (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) == RGN_ALIGN_LEFT) {
    rect_geo.xmax -= ofs_x;
    rect_tex.xmin += 1.0f - alpha_easing;
    alpha = 1.0f;
  }

  /* Not the same layout as #rctf/#rcti. */
  const float rectt[4] = {rect_tex.xmin, rect_tex.ymin, rect_tex.xmax, rect_tex.ymax};
  const float rectg[4] = {
      float(rect_geo.xmin), float(rect_geo.ymin), float(rect_geo.xmax), float(rect_geo.ymax)};

  if (blend) {
    /* Regions drawn off-screen have pre-multiplied alpha. */
    GPU_blend(GPU_BLEND_ALPHA_PREMULT);
  }

  /* setup actual texture */
  GPUTexture *texture = wm_draw_region_texture(region, view);

  GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_2D_IMAGE_RECT_COLOR);
  GPU_shader_bind(shader);

  int color_loc = GPU_shader_get_builtin_uniform(shader, GPU_UNIFORM_COLOR);
  int rect_tex_loc = GPU_shader_get_uniform(shader, "rect_icon");
  int rect_geo_loc = GPU_shader_get_uniform(shader, "rect_geom");
  int texture_bind_loc = GPU_shader_get_sampler_binding(shader, "image");

  GPU_texture_bind(texture, texture_bind_loc);

  GPU_shader_uniform_float_ex(shader, rect_tex_loc, 4, 1, rectt);
  GPU_shader_uniform_float_ex(shader, rect_geo_loc, 4, 1, rectg);
  GPU_shader_uniform_float_ex(shader, color_loc, 4, 1, blender::float4{1, 1, 1, 1});

  GPUBatch *quad = GPU_batch_preset_quad();
  GPU_batch_set_shader(quad, shader);
  GPU_batch_draw(quad);

  GPU_texture_unbind(texture);

  if (blend) {
    GPU_blend(GPU_BLEND_NONE);
  }
}

GPUViewport *WM_draw_region_get_viewport(ARegion *region)
{
  if (!region->draw_buffer) {
    return nullptr;
  }

  GPUViewport *viewport = region->draw_buffer->viewport;
  return viewport;
}

GPUViewport *WM_draw_region_get_bound_viewport(ARegion *region)
{
  if (!region->draw_buffer || region->draw_buffer->bound_view == -1) {
    return nullptr;
  }

  GPUViewport *viewport = region->draw_buffer->viewport;
  return viewport;
}

static void wm_draw_window_offscreen(bContext *C, wmWindow *win, bool stereo)
{
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  bScreen *screen = WM_window_get_active_screen(win);

  /* Draw screen areas into own frame buffer. */
  ED_screen_areas_iter (win, screen, area) {
    CTX_wm_area_set(C, area);
    GPU_debug_group_begin(wm_area_name(area));

    /* Compute UI layouts for dynamically size regions. */
    LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
      if (region->flag & RGN_FLAG_POLL_FAILED) {
        continue;
      }
      /* Dynamic region may have been flagged as too small because their size on init is 0.
       * ARegion.visible is false then, as expected. The layout should still be created then, so
       * the region size can be updated (it may turn out to be not too small then). */
      const bool ignore_visibility = (region->flag & RGN_FLAG_DYNAMIC_SIZE) &&
                                     (region->flag & RGN_FLAG_TOO_SMALL) &&
                                     !(region->flag & RGN_FLAG_HIDDEN);

      if ((region->visible || ignore_visibility) && region->do_draw && region->type &&
          region->type->layout)
      {
        CTX_wm_region_set(C, region);
        ED_region_do_layout(C, region);
        CTX_wm_region_set(C, nullptr);
      }
    }

    ED_area_update_region_sizes(wm, win, area);

    if (area->flag & AREA_FLAG_ACTIVE_TOOL_UPDATE) {
      if ((1 << area->spacetype) & WM_TOOLSYSTEM_SPACE_MASK) {
        WM_toolsystem_update_from_context(
            C, CTX_wm_workspace(C), CTX_data_scene(C), CTX_data_view_layer(C), area);
      }
      area->flag &= ~AREA_FLAG_ACTIVE_TOOL_UPDATE;
    }

    /* Then do actual drawing of regions. */
    LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
      if (!region->visible || !region->do_draw) {
        continue;
      }

      CTX_wm_region_set(C, region);
      bool use_viewport = WM_region_use_viewport(area, region);

      GPU_debug_group_begin(use_viewport ? "Viewport" : "ARegion");

      if (stereo && wm_draw_region_stereo_set(bmain, area, region, STEREO_LEFT_ID)) {
        Scene *scene = WM_window_get_active_scene(win);
        wm_draw_region_buffer_create(scene, region, true, use_viewport);

        for (int view = 0; view < 2; view++) {
          eStereoViews sview;
          if (view == 0) {
            sview = STEREO_LEFT_ID;
          }
          else {
            sview = STEREO_RIGHT_ID;
            wm_draw_region_stereo_set(bmain, area, region, sview);
          }

          wm_draw_region_bind(region, view);
          ED_region_do_draw(C, region);
          wm_draw_region_unbind(region);
        }
        if (use_viewport) {
          GPUViewport *viewport = region->draw_buffer->viewport;
          GPU_viewport_stereo_composite(viewport, win->stereo3d_format);
        }
      }
      else {
        wm_draw_region_stereo_set(bmain, area, region, STEREO_LEFT_ID);
        Scene *scene = WM_window_get_active_scene(win);
        wm_draw_region_buffer_create(scene, region, false, use_viewport);
        wm_draw_region_bind(region, 0);
        ED_region_do_draw(C, region);
        wm_draw_region_unbind(region);
      }

      GPU_debug_group_end();

      region->do_draw = false;
      CTX_wm_region_set(C, nullptr);
    }

    CTX_wm_area_set(C, nullptr);

    GPU_debug_group_end();
  }

  /* Draw menus into their own frame-buffer. */
  LISTBASE_FOREACH (ARegion *, region, &screen->regionbase) {
    if (!region->visible) {
      continue;
    }
    CTX_wm_menu_set(C, region);

    GPU_debug_group_begin("Menu");

    if (region->type && region->type->layout) {
      /* UI code reads the OpenGL state, but we have to refresh
       * the UI layout beforehand in case the menu size changes. */
      wmViewport(&region->winrct);
      region->type->layout(C, region);
    }

    Scene *scene = WM_window_get_active_scene(win);
    wm_draw_region_buffer_create(scene, region, false, false);
    wm_draw_region_bind(region, 0);
    GPU_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    ED_region_do_draw(C, region);
    wm_draw_region_unbind(region);

    GPU_debug_group_end();

    region->do_draw = false;
    CTX_wm_menu_set(C, nullptr);
  }
}

static void wm_draw_window_onscreen(bContext *C, wmWindow *win, int view)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  bScreen *screen = WM_window_get_active_screen(win);

  GPU_debug_group_begin("Window Redraw");

  /* Draw into the window frame-buffer, in full window coordinates. */
  wmWindowViewport(win);

/* We draw on all pixels of the windows so we don't need to clear them before.
 * Actually this is only a problem when resizing the window.
 * If it becomes a problem we should clear only when window size changes. */
#if 0
  GPU_clear_color(0, 0, 0, 0);
#endif

  /* Blit non-overlapping area regions. */
  ED_screen_areas_iter (win, screen, area) {
    LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
      if (!region->visible) {
        continue;
      }

      if (region->overlap == false) {
        /* Blit from off-screen buffer. */
        wm_draw_region_blit(region, view);
      }
    }
  }

  /* Draw overlays and paint cursors. */
  ED_screen_areas_iter (win, screen, area) {
    LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
      if (!region->visible) {
        continue;
      }
      const bool do_paint_cursor = (wm->paintcursors.first && region == screen->active_region);
      const bool do_draw_overlay = (region->type && region->type->draw_overlay);
      if (!(do_paint_cursor || do_draw_overlay)) {
        continue;
      }

      CTX_wm_area_set(C, area);
      CTX_wm_region_set(C, region);
      if (do_draw_overlay) {
        wm_region_draw_overlay(C, area, region);
      }
      if (do_paint_cursor) {
        wm_paintcursor_draw(C, area, region);
      }
      CTX_wm_region_set(C, nullptr);
      CTX_wm_area_set(C, nullptr);
    }
  }
  wmWindowViewport(win);

  /* Blend in overlapping area regions */
  ED_screen_areas_iter (win, screen, area) {
    LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
      if (!region->visible) {
        continue;
      }
      if (region->overlap) {
        wm_draw_region_blend(region, 0, true);
      }
    }
  }

  /* After area regions so we can do area 'overlay' drawing. */
  UI_SetTheme(0, 0);
  ED_screen_draw_edges(win);
  wm_draw_callbacks(win);
  wmWindowViewport(win);

  /* Blend in floating regions (menus). */
  LISTBASE_FOREACH (ARegion *, region, &screen->regionbase) {
    if (!region->visible) {
      continue;
    }
    wm_draw_region_blend(region, 0, true);
  }

  /* always draw, not only when screen tagged */
  if (win->gesture.first) {
    wm_gesture_draw(win);
    wmWindowViewport(win);
  }

  /* Needs pixel coords in screen. */
  if (wm->drags.first) {
    wm_drags_draw(C, win);
    wmWindowViewport(win);
  }

  if (wm_software_cursor_needed()) {
    GrabState grab_state;
    if (wm_software_cursor_needed_for_window(win, &grab_state)) {
      wm_software_cursor_draw(win, &grab_state);
      wm_software_cursor_motion_update(win);
    }
    else {
      /* Checking the window is needed so one window doesn't clear the cursor state of another. */
      wm_software_cursor_motion_clear_with_window(win);
    }
  }

  GPU_debug_group_end();
}

static void wm_draw_window(bContext *C, wmWindow *win)
{
  GPU_context_begin_frame(static_cast<GPUContext *>(win->gpuctx));

  bScreen *screen = WM_window_get_active_screen(win);
  bool stereo = WM_stereo3d_enabled(win, false);

  /* Avoid any BGL call issued before this to alter the window drawin. */
  GPU_bgl_end();

  /* Draw area regions into their own frame-buffer. This way we can redraw
   * the areas that need it, and blit the rest from existing frame-buffers. */
  wm_draw_window_offscreen(C, win, stereo);

  /* Now we draw into the window frame-buffer, in full window coordinates. */
  if (!stereo) {
    /* Regular mono drawing. */
    wm_draw_window_onscreen(C, win, -1);
  }
  else if (win->stereo3d_format->display_mode == S3D_DISPLAY_PAGEFLIP) {
    /* For page-flip we simply draw to both back buffers. */
    GPU_backbuffer_bind(GPU_BACKBUFFER_RIGHT);
    wm_draw_window_onscreen(C, win, 1);

    GPU_backbuffer_bind(GPU_BACKBUFFER_LEFT);
    wm_draw_window_onscreen(C, win, 0);
  }
  else if (ELEM(win->stereo3d_format->display_mode, S3D_DISPLAY_ANAGLYPH, S3D_DISPLAY_INTERLACE)) {
    /* For anaglyph and interlace, we draw individual regions with
     * stereo frame-buffers using different shaders. */
    wm_draw_window_onscreen(C, win, -1);
  }
  else {
    /* Determine desired offscreen format depending on HDR availability. */
    eGPUTextureFormat desired_format = get_hdr_framebuffer_format(WM_window_get_active_scene(win));

    /* For side-by-side and top-bottom, we need to render each view to an
     * an off-screen texture and then draw it. This used to happen for all
     * stereo methods, but it's less efficient than drawing directly. */
    const int width = WM_window_pixels_x(win);
    const int height = WM_window_pixels_y(win);
    GPUOffScreen *offscreen = GPU_offscreen_create(
        width, height, false, desired_format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);

    if (offscreen) {
      GPUTexture *texture = GPU_offscreen_color_texture(offscreen);
      wm_draw_offscreen_texture_parameters(offscreen);

      for (int view = 0; view < 2; view++) {
        /* Draw view into offscreen buffer. */
        GPU_offscreen_bind(offscreen, false);
        wm_draw_window_onscreen(C, win, view);
        GPU_offscreen_unbind(offscreen, false);

        /* Draw offscreen buffer to screen. */
        GPU_texture_bind(texture, 0);

        wmWindowViewport(win);
        if (win->stereo3d_format->display_mode == S3D_DISPLAY_SIDEBYSIDE) {
          wm_stereo3d_draw_sidebyside(win, view);
        }
        else {
          wm_stereo3d_draw_topbottom(win, view);
        }

        GPU_texture_unbind(texture);
      }

      GPU_offscreen_free(offscreen);
    }
    else {
      /* Still draw something in case of allocation failure. */
      wm_draw_window_onscreen(C, win, 0);
    }
  }

  screen->do_draw = false;

  GPU_context_end_frame(static_cast<GPUContext *>(win->gpuctx));
}

/**
 * Draw offscreen contexts not bound to a specific window.
 */
static void wm_draw_surface(bContext *C, wmSurface *surface)
{
  wm_window_clear_drawable(CTX_wm_manager(C));
  wm_surface_make_drawable(surface);

  GPU_context_begin_frame(surface->blender_gpu_context);

  surface->draw(C);

  GPU_context_end_frame(surface->blender_gpu_context);

  /* Avoid interference with window drawable */
  wm_surface_clear_drawable();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Screen Shot Utility (Front-Buffer & Off-Screen)
 *
 * Include here since it can involve low level buffer switching.
 * \{ */

uint8_t *WM_window_pixels_read_from_frontbuffer(const wmWindowManager *wm,
                                                const wmWindow *win,
                                                int r_size[2])
{
  /* Don't assert as file-save uses this for a screenshot, where redrawing isn't an option
   * because of the side-effects of drawing a window on save.
   * In this case the thumbnail might not work and there are currently no better alternatives. */
  // BLI_assert(WM_capabilities_flag() & WM_CAPABILITY_GPU_FRONT_BUFFER_READ);

  /* WARNING: Reading from the front-buffer immediately after drawing may fail,
   * for a slower but more reliable version of this function
   * #WM_window_pixels_read_from_offscreen should be preferred.
   * See it's comments for details on why it's needed, see also #98462. */
  bool setup_context = wm->windrawable != win;

  if (setup_context) {
    GHOST_ActivateWindowDrawingContext(static_cast<GHOST_WindowHandle>(win->ghostwin));
    GPU_context_active_set(static_cast<GPUContext *>(win->gpuctx));
  }

  r_size[0] = WM_window_pixels_x(win);
  r_size[1] = WM_window_pixels_y(win);
  const uint rect_len = r_size[0] * r_size[1];
  uint8_t *rect = static_cast<uint8_t *>(MEM_mallocN(4 * sizeof(uint8_t) * rect_len, __func__));

  GPU_frontbuffer_read_color(0, 0, r_size[0], r_size[1], 4, GPU_DATA_UBYTE, rect);

  if (setup_context) {
    if (wm->windrawable) {
      GHOST_ActivateWindowDrawingContext(
          static_cast<GHOST_WindowHandle>(wm->windrawable->ghostwin));
      GPU_context_active_set(static_cast<GPUContext *>(wm->windrawable->gpuctx));
    }
  }

  /* Clear alpha, it is not set to a meaningful value in OpenGL. */
  uchar *cp = (uchar *)rect;
  uint i;
  for (i = 0, cp += 3; i < rect_len; i++, cp += 4) {
    *cp = 0xff;
  }
  return rect;
}

void WM_window_pixels_read_sample_from_frontbuffer(const wmWindowManager *wm,
                                                   const wmWindow *win,
                                                   const int pos[2],
                                                   float r_col[3])
{
  BLI_assert(WM_capabilities_flag() & WM_CAPABILITY_GPU_FRONT_BUFFER_READ);
  bool setup_context = wm->windrawable != win;

  if (setup_context) {
    GHOST_ActivateWindowDrawingContext(static_cast<GHOST_WindowHandle>(win->ghostwin));
    GPU_context_active_set(static_cast<GPUContext *>(win->gpuctx));
  }

  GPU_frontbuffer_read_color(pos[0], pos[1], 1, 1, 3, GPU_DATA_FLOAT, r_col);

  if (setup_context) {
    if (wm->windrawable) {
      GHOST_ActivateWindowDrawingContext(
          static_cast<GHOST_WindowHandle>(wm->windrawable->ghostwin));
      GPU_context_active_set(static_cast<GPUContext *>(wm->windrawable->gpuctx));
    }
  }
}

uint8_t *WM_window_pixels_read_from_offscreen(bContext *C, wmWindow *win, int r_size[2])
{
  /* NOTE(@ideasman42): There is a problem reading the windows front-buffer after redrawing
   * the window in some cases (typically to clear UI elements such as menus or search popup).
   * With EGL `eglSurfaceAttrib(..)` may support setting the `EGL_SWAP_BEHAVIOR` attribute to
   * `EGL_BUFFER_PRESERVED` however not all implementations support this.
   * Requesting the ability with `EGL_SWAP_BEHAVIOR_PRESERVED_BIT` can even cause the EGL context
   * not to initialize at all.
   * Confusingly there are some cases where this *does* work, depending on the state of the window
   * and prior calls to swap-buffers, however ensuring the state exactly as needed to satisfy a
   * particular GPU back-end is fragile, see #98462.
   *
   * So provide an alternative to #WM_window_pixels_read that avoids using the front-buffer. */

  /* Draw into an off-screen buffer and read its contents. */
  r_size[0] = WM_window_pixels_x(win);
  r_size[1] = WM_window_pixels_y(win);

  /* Determine desired offscreen format depending on HDR availability. */
  eGPUTextureFormat desired_format = get_hdr_framebuffer_format(WM_window_get_active_scene(win));

  GPUOffScreen *offscreen = GPU_offscreen_create(
      r_size[0], r_size[1], false, desired_format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
  if (UNLIKELY(!offscreen)) {
    return nullptr;
  }

  const uint rect_len = r_size[0] * r_size[1];
  uint8_t *rect = static_cast<uint8_t *>(MEM_mallocN(4 * sizeof(uint8_t) * rect_len, __func__));
  GPU_offscreen_bind(offscreen, false);
  wm_draw_window_onscreen(C, win, -1);
  GPU_offscreen_unbind(offscreen, false);
  GPU_offscreen_read_color(offscreen, GPU_DATA_UBYTE, rect);
  GPU_offscreen_free(offscreen);
  return rect;
}

bool WM_window_pixels_read_sample_from_offscreen(bContext *C,
                                                 wmWindow *win,
                                                 const int pos[2],
                                                 float r_col[3])
{
  /* A version of #WM_window_pixels_read_from_offscreen that reads a single sample. */
  const int size[2] = {WM_window_pixels_x(win), WM_window_pixels_y(win)};
  zero_v3(r_col);

  /* While this shouldn't happen, return in the case it does. */
  BLI_assert(uint(pos[0]) < uint(size[0]) && uint(pos[1]) < uint(size[1]));
  if (!(uint(pos[0]) < uint(size[0]) && uint(pos[1]) < uint(size[1]))) {
    return false;
  }

  GPUOffScreen *offscreen = GPU_offscreen_create(
      size[0], size[1], false, GPU_RGBA8, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
  if (UNLIKELY(!offscreen)) {
    return false;
  }

  float rect_pixel[4];
  GPU_offscreen_bind(offscreen, false);
  wm_draw_window_onscreen(C, win, -1);
  GPU_offscreen_unbind(offscreen, false);
  GPU_offscreen_read_color_region(offscreen, GPU_DATA_FLOAT, pos[0], pos[1], 1, 1, rect_pixel);
  GPU_offscreen_free(offscreen);
  copy_v3_v3(r_col, rect_pixel);
  return true;
}

uint8_t *WM_window_pixels_read(bContext *C, wmWindow *win, int r_size[2])
{
  if (WM_capabilities_flag() & WM_CAPABILITY_GPU_FRONT_BUFFER_READ) {
    return WM_window_pixels_read_from_frontbuffer(CTX_wm_manager(C), win, r_size);
  }
  return WM_window_pixels_read_from_offscreen(C, win, r_size);
}

bool WM_window_pixels_read_sample(bContext *C, wmWindow *win, const int pos[2], float r_col[3])
{
  if (WM_capabilities_flag() & WM_CAPABILITY_GPU_FRONT_BUFFER_READ) {
    WM_window_pixels_read_sample_from_frontbuffer(CTX_wm_manager(C), win, pos, r_col);
    return true;
  }
  return WM_window_pixels_read_sample_from_offscreen(C, win, pos, r_col);
}

bool WM_desktop_cursor_sample_read(float r_col[3])
{
  return GHOST_GetPixelAtCursor(r_col);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Update Call
 * \{ */

/* quick test to prevent changing window drawable */
static bool wm_draw_update_test_window(Main *bmain, bContext *C, wmWindow *win)
{
  const wmWindowManager *wm = CTX_wm_manager(C);
  Scene *scene = WM_window_get_active_scene(win);
  ViewLayer *view_layer = WM_window_get_active_view_layer(win);
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
  bScreen *screen = WM_window_get_active_screen(win);
  bool do_draw = false;

  LISTBASE_FOREACH (ARegion *, region, &screen->regionbase) {
    if (region->do_draw_paintcursor) {
      screen->do_draw_paintcursor = true;
      region->do_draw_paintcursor = false;
    }
    if (region->visible && region->do_draw) {
      do_draw = true;
    }
  }

  ED_screen_areas_iter (win, screen, area) {
    LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
      wm_region_test_gizmo_do_draw(C, area, region, true);
      wm_region_test_render_do_draw(scene, depsgraph, area, region);
#ifdef WITH_XR_OPENXR
      wm_region_test_xr_do_draw(wm, area, region);
#endif

      if (region->visible && region->do_draw) {
        do_draw = true;
      }
    }
  }

  if (do_draw) {
    return true;
  }

  if (screen->do_refresh) {
    return true;
  }
  if (screen->do_draw) {
    return true;
  }
  if (screen->do_draw_gesture) {
    return true;
  }
  if (screen->do_draw_paintcursor) {
    return true;
  }
  if (screen->do_draw_drag) {
    return true;
  }

  if (wm_software_cursor_needed()) {
    GrabState grab_state;
    if (wm_software_cursor_needed_for_window(win, &grab_state)) {
      if (wm_software_cursor_motion_test(win)) {
        return true;
      }
    }
    else {
      /* Detect the edge case when the previous draw used the software cursor but this one doesn't,
       * it's important to redraw otherwise the software cursor will remain displayed. */
      if (g_software_cursor.winid == win->winid) {
        return true;
      }
    }
  }

#ifndef WITH_XR_OPENXR
  UNUSED_VARS(wm);
#endif

  return false;
}

/* Clear drawing flags, after drawing is complete so any draw flags set during
 * drawing don't cause any additional redraws. */
static void wm_draw_update_clear_window(bContext *C, wmWindow *win)
{
  bScreen *screen = WM_window_get_active_screen(win);

  ED_screen_areas_iter (win, screen, area) {
    LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
      wm_region_test_gizmo_do_draw(C, area, region, false);
    }
  }

  screen->do_draw_gesture = false;
  screen->do_draw_paintcursor = false;
  screen->do_draw_drag = false;
}

void WM_paint_cursor_tag_redraw(wmWindow *win, ARegion * /*region*/)
{
  if (win) {
    bScreen *screen = WM_window_get_active_screen(win);
    screen->do_draw_paintcursor = true;
  }
}

void wm_draw_update(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = CTX_wm_manager(C);

  GPU_context_main_lock();

  GPU_render_begin();
  GPU_render_step();

  BKE_image_free_unused_gpu_textures();

  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
#ifdef WIN32
    GHOST_TWindowState state = GHOST_GetWindowState(
        static_cast<GHOST_WindowHandle>(win->ghostwin));

    if (state == GHOST_kWindowStateMinimized) {
      /* do not update minimized windows, gives issues on Intel (see #33223)
       * and AMD (see #50856). it seems logical to skip update for invisible
       * window anyway.
       */
      continue;
    }
#endif

    CTX_wm_window_set(C, win);

    if (wm_draw_update_test_window(bmain, C, win)) {
      bScreen *screen = WM_window_get_active_screen(win);

      /* sets context window+screen */
      wm_window_make_drawable(wm, win);

      /* notifiers for screen redraw */
      ED_screen_ensure_updated(C, wm, win, screen);

      wm_draw_window(C, win);
      wm_draw_update_clear_window(C, win);

      wm_window_swap_buffers(win);
    }
  }

  CTX_wm_window_set(C, nullptr);

  /* Draw non-windows (surfaces) */
  wm_surfaces_iter(C, wm_draw_surface);

  GPU_render_end();
  GPU_context_main_unlock();
}

void wm_draw_region_clear(wmWindow *win, ARegion * /*region*/)
{
  bScreen *screen = WM_window_get_active_screen(win);
  screen->do_draw = true;
}

void WM_draw_region_free(ARegion *region, bool hide)
{
  wm_draw_region_buffer_free(region);
  if (hide) {
    region->visible = 0;
  }
}

void wm_draw_region_test(bContext *C, ScrArea *area, ARegion *region)
{
  /* Function for redraw timer benchmark. */
  bool use_viewport = WM_region_use_viewport(area, region);
  wmWindow *win = CTX_wm_window(C);
  Scene *scene = WM_window_get_active_scene(win);
  wm_draw_region_buffer_create(scene, region, false, use_viewport);
  wm_draw_region_bind(region, 0);
  ED_region_do_draw(C, region);
  wm_draw_region_unbind(region);
  region->do_draw = false;
}

void WM_redraw_windows(bContext *C)
{
  wmWindow *win_prev = CTX_wm_window(C);
  ScrArea *area_prev = CTX_wm_area(C);
  ARegion *region_prev = CTX_wm_region(C);

  wm_draw_update(C);

  CTX_wm_window_set(C, win_prev);
  CTX_wm_area_set(C, area_prev);
  CTX_wm_region_set(C, region_prev);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Viewport Drawing
 *
 * This is needed for viewport drawing for operator use
 * (where the viewport may not have drawn yet).
 *
 * Otherwise avoid using these since they're exposing low level logic externally.
 *
 * \{ */

void WM_draw_region_viewport_ensure(Scene *scene, ARegion *region, short space_type)
{
  bool use_viewport = wm_region_use_viewport_by_type(space_type, region->regiontype);
  wm_draw_region_buffer_create(scene, region, false, use_viewport);
}

void WM_draw_region_viewport_bind(ARegion *region)
{
  wm_draw_region_bind(region, 0);
}

void WM_draw_region_viewport_unbind(ARegion *region)
{
  wm_draw_region_unbind(region);
}

/** \} */
