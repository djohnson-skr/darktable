/*
    This file is part of darktable,
    Copyright (C) 2024-2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * find_duplicates.c — Duplicate image detection for darktable's lighttable.
 *
 * Algorithm
 * ─────────
 * 1.  Exact duplicates   : images sharing the same SHA-1 checksum stored in
 *     the "images" table are flagged immediately without thumbnail access.
 *
 * 2.  Near-duplicate (perceptual) detection via difference-hash (dHash):
 *       • Load a small thumbnail (DT_MIPMAP_1) for each image.
 *       • Down-sample to a 9×8 grayscale grid.
 *       • Produce a 64-bit dHash by comparing adjacent horizontal pixels.
 *       • Pair images whose Hamming distance ≤ threshold (default 10/64).
 *       • Use Union-Find to assemble transitive groups.
 *
 * 3.  Review window opens after detection with:
 *       • Scrollable list of duplicate groups with thumbnails.
 *       • Per-image "delete" checkboxes (one image per group kept by default).
 *       • "Delete Selected" and "Select All Duplicates (Keep Best)" buttons.
 */

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <string.h>

DT_MODULE(1)

/* ─── thumbnail display size in the review dialog (pixels) ─────────────── */
#define FD_THUMB_SIZE     160
/* ─── dHash grid dimensions ─────────────────────────────────────────────── */
#define FD_HASH_COLS      9
#define FD_HASH_ROWS      8
/* ─── default Hamming-distance threshold (bits out of 64) ───────────────── */
#define FD_DEFAULT_THR    10

/* ═══════════════════════════════════════════════════════════════════════════
 * Data structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* One group of visually similar images (2+ members). */
typedef struct _dup_group_t
{
  GList    *imgids;    /* GList of GINT_TO_POINTER(dt_imgid_t)              */
  gboolean  is_exact;  /* TRUE → same SHA-1 content hash                   */
  float     similarity; /* 1.0 = identical, decreasing toward 0            */
} _dup_group_t;

/* Module private data (attached to the lib panel). */
typedef struct dt_lib_find_duplicates_t
{
  GtkWidget *detect_button;
  GtkWidget *status_label;
  GtkWidget *threshold_spin;
} dt_lib_find_duplicates_t;

/* Payload passed to the background detection thread. */
typedef struct _detect_params_t
{
  dt_lib_module_t *self;
  GList           *all_ids;   /* pre-queried from main thread               */
  int              threshold;
} _detect_params_t;

/* Payload delivered back to the main thread via gdk_threads_add_idle(). */
typedef struct _show_dialog_data_t
{
  dt_lib_module_t *self;
  GList           *groups;   /* list of _dup_group_t *                      */
  int              n_images;
  int              n_skipped;
} _show_dialog_data_t;

/* Per-image row state inside the review dialog. */
typedef struct _img_row_t
{
  dt_imgid_t  imgid;
  GtkWidget  *check; /* GtkCheckButton                                      */
} _img_row_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Plugin meta-data
 * ═══════════════════════════════════════════════════════════════════════════ */

const char *name(dt_lib_module_t *self)
{
  return _("find duplicates");
}

const char *description(dt_lib_module_t *self)
{
  return _("detect exact and near-duplicate images\nin the current collection");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 750; /* sit below "selection" (800) */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Perceptual hashing — difference hash (dHash)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Compute a 64-bit dHash from an RGBA-8 thumbnail buffer.
 *
 * Steps:
 *   1. Bilinearly down-sample to 9 × 8 grayscale cells.
 *   2. For each of the 8 rows compare pixel[col] vs pixel[col+1].
 *      Bit = 1 when left < right (brightness increases rightwards).
 *   3. Pack 64 bits.
 */
static uint64_t _compute_dhash(const uint8_t *rgba_buf, int width, int height)
{
  if(!rgba_buf || width <= 0 || height <= 0) return 0;

  float gray[FD_HASH_COLS * FD_HASH_ROWS];

  for(int row = 0; row < FD_HASH_ROWS; row++)
  {
    for(int col = 0; col < FD_HASH_COLS; col++)
    {
      float sx = (col + 0.5f) * (float)width  / (float)FD_HASH_COLS - 0.5f;
      float sy = (row + 0.5f) * (float)height / (float)FD_HASH_ROWS - 0.5f;

      int x0 = CLAMP((int)sx,      0, width  - 1);
      int y0 = CLAMP((int)sy,      0, height - 1);
      int x1 = CLAMP(x0 + 1,       0, width  - 1);
      int y1 = CLAMP(y0 + 1,       0, height - 1);

      float fx = sx - (float)x0;
      float fy = sy - (float)y0;

      const uint8_t *p00 = rgba_buf + (y0 * width + x0) * 4;
      const uint8_t *p01 = rgba_buf + (y0 * width + x1) * 4;
      const uint8_t *p10 = rgba_buf + (y1 * width + x0) * 4;
      const uint8_t *p11 = rgba_buf + (y1 * width + x1) * 4;

      /* ITU-R BT.601 luminance */
      float l00 = 0.299f * p00[0] + 0.587f * p00[1] + 0.114f * p00[2];
      float l01 = 0.299f * p01[0] + 0.587f * p01[1] + 0.114f * p01[2];
      float l10 = 0.299f * p10[0] + 0.587f * p10[1] + 0.114f * p10[2];
      float l11 = 0.299f * p11[0] + 0.587f * p11[1] + 0.114f * p11[2];

      gray[row * FD_HASH_COLS + col] =
          (1.0f - fx) * (1.0f - fy) * l00
        + fx           * (1.0f - fy) * l01
        + (1.0f - fx) * fy           * l10
        + fx           * fy           * l11;
    }
  }

  uint64_t hash = 0;
  for(int row = 0; row < FD_HASH_ROWS; row++)
    for(int col = 0; col < FD_HASH_ROWS; col++) /* FD_HASH_ROWS==8 bits per row */
      if(gray[row * FD_HASH_COLS + col] < gray[row * FD_HASH_COLS + col + 1])
        hash |= (uint64_t)1 << (row * FD_HASH_ROWS + col);

  return hash;
}

static int _popcount64(uint64_t x)
{
  int n = 0;
  while(x) { n += (int)(x & 1); x >>= 1; }
  return n;
}

static inline int _hamming(uint64_t a, uint64_t b)
{
  return _popcount64(a ^ b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Union-Find (disjoint-set) for transitive grouping
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct { int *parent; int *rank; int n; } _uf_t;

static _uf_t *_uf_new(int n)
{
  _uf_t *uf    = g_malloc(sizeof(_uf_t));
  uf->n        = n;
  uf->parent   = g_malloc_n(n, sizeof(int));
  uf->rank     = g_malloc0_n(n, sizeof(int));
  for(int i = 0; i < n; i++) uf->parent[i] = i;
  return uf;
}

static int _uf_find(_uf_t *uf, int x)
{
  while(uf->parent[x] != x)
  {
    uf->parent[x] = uf->parent[uf->parent[x]]; /* path halving */
    x = uf->parent[x];
  }
  return x;
}

static void _uf_union(_uf_t *uf, int x, int y)
{
  int rx = _uf_find(uf, x);
  int ry = _uf_find(uf, y);
  if(rx == ry) return;
  if(uf->rank[rx] < uf->rank[ry]) { int t = rx; rx = ry; ry = t; }
  uf->parent[ry] = rx;
  if(uf->rank[rx] == uf->rank[ry]) uf->rank[rx]++;
}

static void _uf_free(_uf_t *uf)
{
  g_free(uf->parent);
  g_free(uf->rank);
  g_free(uf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _free_groups(GList *groups)
{
  for(GList *g = groups; g; g = g->next)
  {
    _dup_group_t *grp = (_dup_group_t *)g->data;
    g_list_free(grp->imgids);
    g_free(grp);
  }
  g_list_free(groups);
}

/*
 * Build a scaled GdkPixbuf for one image to display in the dialog.
 * Returns NULL if no cached thumbnail is available.
 * Must be called from the GTK main thread.
 */
static GdkPixbuf *_make_thumbnail_pixbuf(dt_imgid_t imgid)
{
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_2, DT_MIPMAP_BEST_EFFORT, 'r');

  if(!buf.buf)
  {
    dt_mipmap_cache_release(&buf);
    dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_1, DT_MIPMAP_BEST_EFFORT, 'r');
  }
  if(!buf.buf)
  {
    dt_mipmap_cache_release(&buf);
    dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_0, DT_MIPMAP_BEST_EFFORT, 'r');
  }

  GdkPixbuf *pixbuf = NULL;

  if(buf.buf && buf.width > 0 && buf.height > 0)
  {
    /* Copy buffer so we can set alpha = 255 without touching the cache. */
    gsize sz  = (gsize)buf.width * buf.height * 4;
    uint8_t *tmp = g_malloc(sz);
    memcpy(tmp, buf.buf, sz);
    for(gsize i = 3; i < sz; i += 4)
      tmp[i] = 0xFF;

    GdkPixbuf *raw = gdk_pixbuf_new_from_data(
        tmp, GDK_COLORSPACE_RGB, TRUE, 8,
        buf.width, buf.height, buf.width * 4,
        (GdkPixbufDestroyNotify)g_free, NULL);

    /* Scale to fit inside FD_THUMB_SIZE × FD_THUMB_SIZE, keep aspect. */
    int tw, th;
    if(buf.width >= buf.height)
    {
      tw = FD_THUMB_SIZE;
      th = MAX(1, (buf.height * FD_THUMB_SIZE) / buf.width);
    }
    else
    {
      th = FD_THUMB_SIZE;
      tw = MAX(1, (buf.width * FD_THUMB_SIZE) / buf.height);
    }

    pixbuf = gdk_pixbuf_scale_simple(raw, tw, th, GDK_INTERP_BILINEAR);
    g_object_unref(raw);
  }

  dt_mipmap_cache_release(&buf);
  return pixbuf;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Review dialog callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Called when "Delete Selected" is clicked.
 * user_data = GList of _img_row_t * (flat list across all groups). */
static void _on_delete_selected(GtkWidget *btn, gpointer user_data)
{
  GList *all_rows = (GList *)user_data;

  GList *to_delete = NULL;
  for(GList *r = all_rows; r; r = r->next)
  {
    _img_row_t *row = (_img_row_t *)r->data;
    if(gtk_widget_is_sensitive(row->check)
       && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(row->check)))
    {
      to_delete = g_list_prepend(to_delete, GINT_TO_POINTER(row->imgid));
    }
  }

  if(!to_delete)
  {
    dt_control_log(_("no images marked for deletion"));
    return;
  }

  int n = g_list_length(to_delete);

  gchar *msg = g_strdup_printf(
      ngettext("Remove %d duplicate from the library?\n(file is not deleted from disk)",
               "Remove %d duplicates from the library?\n(files are not deleted from disk)",
               n),
      n);
  GtkWidget *win    = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dlg    = gtk_message_dialog_new(
      GTK_WINDOW(win),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", msg);
  g_free(msg);
  gtk_window_set_title(GTK_WINDOW(dlg), _("Confirm removal"));
  int resp = gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);

  if(resp != GTK_RESPONSE_YES)
  {
    g_list_free(to_delete);
    return;
  }

  for(GList *d = to_delete; d; d = d->next)
    dt_image_remove(GPOINTER_TO_INT(d->data));
  g_list_free(to_delete);

  /* Grey out processed checkboxes so the user can see what was done. */
  for(GList *r = all_rows; r; r = r->next)
  {
    _img_row_t *row = (_img_row_t *)r->data;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(row->check)))
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->check), FALSE);
      gtk_widget_set_sensitive(row->check, FALSE);
    }
  }

  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD,
                             DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_queue_redraw_center();
  dt_control_log(ngettext("removed %d duplicate", "removed %d duplicates", n), n);
}

/* Called when "Select All Duplicates to Delete (Keep Best)" is clicked.
 * user_data = GList of GList* of _img_row_t * (one inner list per group). */
static void _on_select_all_duplicates(GtkWidget *btn, gpointer user_data)
{
  GList *all_group_rows = (GList *)user_data;

  for(GList *g = all_group_rows; g; g = g->next)
  {
    GList *group = (GList *)g->data;
    gboolean first = TRUE;
    for(GList *r = group; r; r = r->next)
    {
      _img_row_t *row = (_img_row_t *)r->data;
      if(gtk_widget_is_sensitive(row->check))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->check), !first);
      first = FALSE;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Review dialog construction
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _show_review_dialog(dt_lib_module_t *self,
                                GList *groups,
                                int    n_images,
                                int    n_skipped)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  int n_groups   = g_list_length(groups);

  /* ── Top-level window ─────────────────────────────────────────────────── */
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      _("Duplicate Images"),
      GTK_WINDOW(win),
      GTK_DIALOG_DESTROY_WITH_PARENT,
      _("_Close"), GTK_RESPONSE_CLOSE,
      NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 950, 640);
  gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 8);
  gtk_box_set_spacing(GTK_BOX(content), 6);

  /* ── Summary ─────────────────────────────────────────────────────────── */
  gchar *summary_str;
  if(n_groups == 0)
  {
    summary_str = g_strdup_printf(_("No duplicates found (scanned %d images)."), n_images);
  }
  else
  {
    if(n_skipped > 0)
      summary_str = g_strdup_printf(
          ngettext("Found %d duplicate group in %d images (%d skipped — no thumbnail yet).",
                   "Found %d duplicate groups in %d images (%d skipped — no thumbnail yet).",
                   n_groups),
          n_groups, n_images, n_skipped);
    else
      summary_str = g_strdup_printf(
          ngettext("Found %d duplicate group in %d images.",
                   "Found %d duplicate groups in %d images.",
                   n_groups),
          n_groups, n_images);
  }
  GtkWidget *sum_lbl = gtk_label_new(summary_str);
  g_free(summary_str);
  gtk_label_set_line_wrap(GTK_LABEL(sum_lbl), TRUE);
  gtk_widget_set_halign(sum_lbl, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(content), sum_lbl, FALSE, FALSE, 0);

  if(n_groups == 0)
  {
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  /* ── Action buttons ──────────────────────────────────────────────────── */
  GtkWidget *btn_row  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *btn_keep = gtk_button_new_with_label(
      _("Select All Duplicates to Delete (Keep Best)"));
  GtkWidget *btn_del  = gtk_button_new_with_label(_("Delete Selected"));

  GtkStyleContext *del_ctx = gtk_widget_get_style_context(btn_del);
  gtk_style_context_add_class(del_ctx, "destructive-action");

  gtk_widget_set_tooltip_text(btn_keep,
      _("For every group, pre-tick all images except the first one for deletion."));
  gtk_widget_set_tooltip_text(btn_del,
      _("Remove all ticked images from the darktable library (files stay on disk)."));

  gtk_box_pack_start(GTK_BOX(btn_row), btn_keep, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(btn_row), btn_del,  FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), btn_row, FALSE, FALSE, 0);

  /* ── Scrollable groups list ──────────────────────────────────────────── */
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

  GtkWidget *groups_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width(GTK_CONTAINER(groups_box), 6);
  gtk_container_add(GTK_CONTAINER(scroll), groups_box);

  GList *all_rows       = NULL; /* flat, for "Delete Selected"     */
  GList *all_group_rows = NULL; /* per-group, for "Keep Best"      */

  int grp_idx = 0;
  for(GList *g = groups; g; g = g->next, grp_idx++)
  {
    _dup_group_t *grp    = (_dup_group_t *)g->data;
    int           n_imgs = g_list_length(grp->imgids);

    /* ── Frame for one group ─────────────────────────────────────────── */
    gchar *frame_lbl;
    if(grp->is_exact)
      frame_lbl = g_strdup_printf(
          ngettext("Group %d — %d exact duplicate (identical file content)",
                   "Group %d — %d exact duplicates (identical file content)",
                   n_imgs),
          grp_idx + 1, n_imgs);
    else
      frame_lbl = g_strdup_printf(
          ngettext("Group %d — %d similar image (%.0f%% visual similarity)",
                   "Group %d — %d similar images (%.0f%% visual similarity)",
                   n_imgs),
          grp_idx + 1, n_imgs, grp->similarity * 100.0f);

    GtkWidget *frame = gtk_frame_new(frame_lbl);
    g_free(frame_lbl);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_box_pack_start(GTK_BOX(groups_box), frame, FALSE, FALSE, 0);

    GtkWidget *frame_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(frame_vbox), 6);
    gtk_container_add(GTK_CONTAINER(frame), frame_vbox);

    /* Horizontal scrollable strip of image cards */
    GtkWidget *hscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(hscroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(hscroll), FD_THUMB_SIZE + 80);
    gtk_box_pack_start(GTK_BOX(frame_vbox), hscroll, FALSE, FALSE, 0);

    GtkWidget *img_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(img_hbox), 4);
    gtk_container_add(GTK_CONTAINER(hscroll), img_hbox);

    GList *this_group_rows = NULL;
    gboolean first_img     = TRUE;

    for(GList *item = grp->imgids; item; item = item->next)
    {
      dt_imgid_t imgid = GPOINTER_TO_INT(item->data);

      /* ── Single image card ─────────────────────────────────────────── */
      GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_start(card, 4);
      gtk_widget_set_margin_end(card,   4);
      gtk_widget_set_margin_top(card,   2);
      gtk_widget_set_margin_bottom(card, 2);

      /* Thumbnail */
      GdkPixbuf *pb = _make_thumbnail_pixbuf(imgid);
      GtkWidget *img_w;
      if(pb)
      {
        img_w = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
      }
      else
      {
        img_w = gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_DIALOG);
      }
      gtk_widget_set_size_request(img_w, FD_THUMB_SIZE, FD_THUMB_SIZE);
      gtk_box_pack_start(GTK_BOX(card), img_w, FALSE, FALSE, 0);

      /* Filename */
      char path[PATH_MAX] = "";
      dt_image_full_path(imgid, path, sizeof(path), NULL);
      gchar *basename = g_path_get_basename(path);
      GtkWidget *name_lbl = gtk_label_new(basename);
      g_free(basename);
      gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_set_size_request(name_lbl, FD_THUMB_SIZE, -1);
      gtk_box_pack_start(GTK_BOX(card), name_lbl, FALSE, FALSE, 0);

      /* "Keep" label for the first image */
      if(first_img)
      {
        GtkWidget *keep_lbl = gtk_label_new(_("keep (best)"));
        GtkStyleContext *lctx = gtk_widget_get_style_context(keep_lbl);
        gtk_style_context_add_class(lctx, "success");
        gtk_box_pack_start(GTK_BOX(card), keep_lbl, FALSE, FALSE, 0);
      }

      /* Deletion checkbox */
      GtkWidget *chk = gtk_check_button_new_with_label(_("mark for deletion"));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), !first_img);
      gtk_box_pack_start(GTK_BOX(card), chk, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(img_hbox), card, FALSE, FALSE, 0);

      _img_row_t *row = g_malloc(sizeof(_img_row_t));
      row->imgid = imgid;
      row->check = chk;

      all_rows       = g_list_prepend(all_rows, row);
      this_group_rows = g_list_prepend(this_group_rows, row);

      first_img = FALSE;
    }

    this_group_rows  = g_list_reverse(this_group_rows);
    all_group_rows   = g_list_prepend(all_group_rows, this_group_rows);
  }

  all_rows       = g_list_reverse(all_rows);
  all_group_rows = g_list_reverse(all_group_rows);

  g_signal_connect(btn_del,  "clicked", G_CALLBACK(_on_delete_selected),     all_rows);
  g_signal_connect(btn_keep, "clicked", G_CALLBACK(_on_select_all_duplicates), all_group_rows);

  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  /* ── Cleanup ─────────────────────────────────────────────────────────── */
  for(GList *r = all_rows; r; r = r->next) g_free(r->data);
  g_list_free(all_rows);
  for(GList *gg = all_group_rows; gg; gg = gg->next) g_list_free(gg->data);
  g_list_free(all_group_rows);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Detection — background thread
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Idle callback: switch back to the main thread to show the dialog. */
static gboolean _show_dialog_idle(gpointer user_data)
{
  _show_dialog_data_t *d = (_show_dialog_data_t *)user_data;
  dt_lib_find_duplicates_t *md = (dt_lib_find_duplicates_t *)d->self->data;

  gtk_widget_set_sensitive(md->detect_button, TRUE);
  gtk_label_set_text(GTK_LABEL(md->status_label), "");

  _show_review_dialog(d->self, d->groups, d->n_images, d->n_skipped);

  _free_groups(d->groups);
  g_free(d);
  return G_SOURCE_REMOVE;
}

/* Main detection logic — runs entirely off the GTK main thread. */
static gpointer _detect_thread(gpointer user_data)
{
  _detect_params_t *params    = (_detect_params_t *)user_data;
  GList            *ids       = params->all_ids;
  int               threshold = params->threshold;
  int               n         = g_list_length(ids);

  /* ── Per-image arrays ─────────────────────────────────────────────────── */
  dt_imgid_t *img_arr   = g_malloc_n(n, sizeof(dt_imgid_t));
  uint64_t   *hash_arr  = g_malloc_n(n, sizeof(uint64_t));
  char      **sha1_arr  = g_malloc0_n(n, sizeof(char *));
  int         n_skipped = 0;

  int idx = 0;
  for(GList *l = ids; l; l = l->next, idx++)
  {
    dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
    img_arr[idx] = imgid;

    /* SHA-1 exact hash from the database ---------------------------------- */
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
          "SELECT sha1sum FROM main.images WHERE id=?1 AND sha1sum IS NOT NULL",
          -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if(s && s[0]) sha1_arr[idx] = g_strdup(s);
      }
      sqlite3_finalize(stmt);
    }

    /* Perceptual hash from thumbnail -------------------------------------- */
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_1, DT_MIPMAP_BEST_EFFORT, 'r');
    if(!buf.buf)
    {
      dt_mipmap_cache_release(&buf);
      dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_0, DT_MIPMAP_BEST_EFFORT, 'r');
    }

    if(buf.buf && buf.width > 0 && buf.height > 0)
      hash_arr[idx] = _compute_dhash(buf.buf, buf.width, buf.height);
    else
    {
      hash_arr[idx] = 0; /* no thumbnail cached; will be skipped in compare */
      n_skipped++;
    }

    dt_mipmap_cache_release(&buf);
  }

  /* ── Union-Find grouping ─────────────────────────────────────────────── */
  _uf_t *uf = _uf_new(n);

  for(int i = 0; i < n - 1; i++)
  {
    for(int j = i + 1; j < n; j++)
    {
      /* Exact content match via SHA-1 ------------------------------------ */
      if(sha1_arr[i] && sha1_arr[j]
         && strcmp(sha1_arr[i], sha1_arr[j]) == 0)
      {
        _uf_union(uf, i, j);
        continue;
      }

      /* Near-duplicate via perceptual hash -------------------------------- */
      if(hash_arr[i] == 0 || hash_arr[j] == 0)
        continue;

      if(_hamming(hash_arr[i], hash_arr[j]) <= threshold)
        _uf_union(uf, i, j);
    }
  }

  /* ── Collect groups ──────────────────────────────────────────────────── */
  GHashTable *root_map = g_hash_table_new(g_direct_hash, g_direct_equal);

  for(int i = 0; i < n; i++)
  {
    int   root = _uf_find(uf, i);
    GList *lst  = (GList *)g_hash_table_lookup(root_map, GINT_TO_POINTER(root));
    lst = g_list_prepend(lst, GINT_TO_POINTER(img_arr[i]));
    g_hash_table_insert(root_map, GINT_TO_POINTER(root), lst);
  }

  GList *groups = NULL;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, root_map);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    GList *members = (GList *)value;
    if(g_list_length(members) < 2) { g_list_free(members); continue; }

    members = g_list_reverse(members);

    int root_idx = GPOINTER_TO_INT(key);

    /* Determine if all members share the exact SHA-1 hash. */
    gboolean is_exact = FALSE;
    if(sha1_arr[root_idx])
    {
      guint exact_cnt = 0;
      for(GList *m = members; m; m = m->next)
      {
        dt_imgid_t mid = GPOINTER_TO_INT(m->data);
        for(int k = 0; k < n; k++)
          if(img_arr[k] == mid && sha1_arr[k]
             && strcmp(sha1_arr[k], sha1_arr[root_idx]) == 0)
          { exact_cnt++; break; }
      }
      if(exact_cnt == g_list_length(members)) is_exact = TRUE;
    }

    /* Average pairwise visual similarity. */
    float sim_sum = 0.0f;
    int   sim_cnt = 0;
    for(GList *a = members; a; a = a->next)
    {
      for(GList *b = a->next; b; b = b->next)
      {
        uint64_t ha = 0, hb = 0;
        dt_imgid_t ia = GPOINTER_TO_INT(a->data);
        dt_imgid_t ib = GPOINTER_TO_INT(b->data);
        for(int k = 0; k < n; k++)
        {
          if(img_arr[k] == ia) ha = hash_arr[k];
          if(img_arr[k] == ib) hb = hash_arr[k];
        }
        if(ha && hb) { sim_sum += 1.0f - (float)_hamming(ha, hb) / 64.0f; sim_cnt++; }
      }
    }

    _dup_group_t *grp = g_malloc(sizeof(_dup_group_t));
    grp->imgids     = members;
    grp->is_exact   = is_exact;
    grp->similarity = (sim_cnt > 0) ? sim_sum / (float)sim_cnt : 1.0f;
    groups = g_list_prepend(groups, grp);
  }

  groups = g_list_reverse(groups);
  g_hash_table_destroy(root_map);
  _uf_free(uf);

  /* ── Free temporary arrays ───────────────────────────────────────────── */
  for(int i = 0; i < n; i++) g_free(sha1_arr[i]);
  g_free(sha1_arr);
  g_free(hash_arr);
  g_free(img_arr);
  g_list_free(ids);

  /* ── Deliver results to the main thread ──────────────────────────────── */
  _show_dialog_data_t *dlg = g_malloc(sizeof(_show_dialog_data_t));
  dlg->self      = params->self;
  dlg->groups    = groups;
  dlg->n_images  = n;
  dlg->n_skipped = n_skipped;
  gdk_threads_add_idle(_show_dialog_idle, dlg);

  g_free(params);
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Threshold spin-button callback
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _on_threshold_changed(GtkSpinButton *sb, gpointer user_data)
{
  dt_lib_find_duplicates_t *d = (dt_lib_find_duplicates_t *)user_data;
  d->threshold_spin = GTK_WIDGET(sb); /* keep reference, already set */
  /* The threshold is read fresh from the spin button when detection starts,
   * but we also cache it here for quick access. */
  (void)d; /* accessed via widget at detection time */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * "Detect Duplicates" button callback
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _on_detect_clicked(GtkWidget *btn, gpointer user_data)
{
  dt_lib_module_t          *self = (dt_lib_module_t *)user_data;
  dt_lib_find_duplicates_t *d    = (dt_lib_find_duplicates_t *)self->data;

  gtk_widget_set_sensitive(d->detect_button, FALSE);
  gtk_label_set_text(GTK_LABEL(d->status_label), _("Detecting duplicates…"));

  /* Collect image IDs from the current collection on the main thread.
   * The 'memory.collected_images' virtual table is populated by darktable
   * whenever the collection changes and is safe to query directly. */
  GList *all_ids = NULL;
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "SELECT imgid FROM memory.collected_images", -1, &stmt, NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
      all_ids = g_list_prepend(all_ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
    sqlite3_finalize(stmt);
    all_ids = g_list_reverse(all_ids);
  }

  if(!all_ids)
  {
    gtk_widget_set_sensitive(d->detect_button, TRUE);
    gtk_label_set_text(GTK_LABEL(d->status_label), "");
    dt_control_log(_("no images in the current collection"));
    return;
  }

  int thr = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->threshold_spin));

  _detect_params_t *params = g_malloc(sizeof(_detect_params_t));
  params->self      = self;
  params->all_ids   = all_ids;
  params->threshold = thr;

  g_thread_new("dt-find-dupes", _detect_thread, params);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Panel GUI
 * ═══════════════════════════════════════════════════════════════════════════ */

void gui_init(dt_lib_module_t *self)
{
  dt_lib_find_duplicates_t *d = g_malloc0(sizeof(dt_lib_find_duplicates_t));
  self->data = d;

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  self->widget    = vbox;

  /* ── Sensitivity row ─────────────────────────────────────────────────── */
  GtkWidget *thr_row  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *thr_lbl  = gtk_label_new(_("Sensitivity:"));
  gtk_widget_set_tooltip_text(thr_lbl,
      _("Maximum bit-difference between two perceptual hashes (0–32).\n"
        "Lower = only nearly-identical images matched.\n"
        "Higher = more aggressive duplicate grouping."));

  GtkWidget *thr_spin = gtk_spin_button_new_with_range(0.0, 32.0, 1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(thr_spin), (double)FD_DEFAULT_THR);
  gtk_widget_set_tooltip_text(thr_spin,
      _("Hamming distance threshold (bits out of 64).\n"
        "Default 10 is a good starting point."));

  d->threshold_spin = thr_spin;
  g_signal_connect(thr_spin, "value-changed",
                   G_CALLBACK(_on_threshold_changed), d);

  gtk_box_pack_start(GTK_BOX(thr_row), thr_lbl,  FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(thr_row), thr_spin, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox),    thr_row,  FALSE, FALSE, 0);

  /* ── Detect button ───────────────────────────────────────────────────── */
  d->detect_button = gtk_button_new_with_label(_("Detect Duplicates"));
  gtk_widget_set_tooltip_text(d->detect_button,
      _("Scan the current collection for duplicate images.\n"
        "Uses perceptual hashing (dHash) for visually similar photos\n"
        "and SHA-1 checksums for exact byte-for-byte copies.\n"
        "Results open in a review window."));
  g_signal_connect(d->detect_button, "clicked",
                   G_CALLBACK(_on_detect_clicked), self);
  gtk_box_pack_start(GTK_BOX(vbox), d->detect_button, FALSE, FALSE, 0);

  /* ── Status label ────────────────────────────────────────────────────── */
  d->status_label = gtk_label_new("");
  gtk_widget_set_halign(d->status_label, GTK_ALIGN_START);
  gtk_label_set_line_wrap(GTK_LABEL(d->status_label), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), d->status_label, FALSE, FALSE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
