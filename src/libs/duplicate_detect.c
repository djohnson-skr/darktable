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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE(1)

#define DHASH_SIZE 8
#define DHASH_BITS (DHASH_SIZE * DHASH_SIZE)
#define HAMMING_THRESHOLD 12
#define TIMESTAMP_THRESHOLD_SEC 5

typedef struct dt_lib_duplicate_detect_t
{
  GtkWidget *detect_button;
} dt_lib_duplicate_detect_t;

typedef struct _dhash_entry_t
{
  dt_imgid_t imgid;
  uint64_t dhash;
  gboolean has_hash;
  char filename[PATH_MAX];
  char sha1[41];
  char datetime[20];
  char maker[64];
  char model[128];
  int width;
  int height;
} _dhash_entry_t;

typedef struct _dup_group_t
{
  GList *entries;
} _dup_group_t;

typedef struct _dup_dialog_data_t
{
  GtkWidget *dialog;
  GtkWidget *content_box;
  GList *groups;
  GList *check_buttons;
  GtkWidget *status_label;
} _dup_dialog_data_t;

typedef struct _check_entry_t
{
  GtkWidget *check;
  dt_imgid_t imgid;
  gboolean is_keeper;
} _check_entry_t;

const char *name(dt_lib_module_t *self)
{
  return _("duplicate detection");
}

const char *description(dt_lib_module_t *self)
{
  return _("detect and manage duplicate\n"
           "or similar images");
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
  return 850;
}

static int _popcount64(uint64_t x)
{
  x = x - ((x >> 1) & 0x5555555555555555ULL);
  x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
  x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
  return (int)((x * 0x0101010101010101ULL) >> 56);
}

static int _hamming_distance(uint64_t a, uint64_t b)
{
  return _popcount64(a ^ b);
}

static uint64_t _compute_dhash(const uint8_t *buf, int buf_width, int buf_height)
{
  if(!buf || buf_width < 2 || buf_height < 2) return 0;

  float gray[9 * 8];

  for(int y = 0; y < 8; y++)
  {
    for(int x = 0; x < 9; x++)
    {
      const int src_x = (x * (buf_width - 1)) / 8;
      const int src_y = (y * (buf_height - 1)) / 7;
      const int clamped_x = CLAMP(src_x, 0, buf_width - 1);
      const int clamped_y = CLAMP(src_y, 0, buf_height - 1);
      const int offset = (clamped_y * buf_width + clamped_x) * 4;
      const float r = buf[offset + 0];
      const float g = buf[offset + 1];
      const float b = buf[offset + 2];
      gray[y * 9 + x] = 0.299f * r + 0.587f * g + 0.114f * b;
    }
  }

  uint64_t hash = 0;
  for(int y = 0; y < 8; y++)
  {
    for(int x = 0; x < 8; x++)
    {
      if(gray[y * 9 + x] < gray[y * 9 + x + 1])
        hash |= (1ULL << (y * 8 + x));
    }
  }

  return hash;
}

static void _get_image_info(_dhash_entry_t *entry)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "SELECT filename, COALESCE(sha1sum, ''), COALESCE(datetime_taken, ''),"
    " COALESCE(maker, ''), COALESCE(model, ''), width, height"
    " FROM main.images WHERE id = ?1",
    -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, entry->imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_strlcpy(entry->filename, (const char *)sqlite3_column_text(stmt, 0), sizeof(entry->filename));
    g_strlcpy(entry->sha1, (const char *)sqlite3_column_text(stmt, 1), sizeof(entry->sha1));
    g_strlcpy(entry->datetime, (const char *)sqlite3_column_text(stmt, 2), sizeof(entry->datetime));
    g_strlcpy(entry->maker, (const char *)sqlite3_column_text(stmt, 3), sizeof(entry->maker));
    g_strlcpy(entry->model, (const char *)sqlite3_column_text(stmt, 4), sizeof(entry->model));
    entry->width = sqlite3_column_int(stmt, 5);
    entry->height = sqlite3_column_int(stmt, 6);
  }
  sqlite3_finalize(stmt);
}

static void _compute_image_dhash(_dhash_entry_t *entry)
{
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, entry->imgid, DT_MIPMAP_2, DT_MIPMAP_BLOCKING, 'r');

  if(buf.buf && buf.width > 0 && buf.height > 0)
  {
    entry->dhash = _compute_dhash(buf.buf, buf.width, buf.height);
    entry->has_hash = TRUE;
  }
  else
  {
    entry->dhash = 0;
    entry->has_hash = FALSE;
  }

  dt_mipmap_cache_release(&buf);
}

static gboolean _are_potential_duplicates(const _dhash_entry_t *a, const _dhash_entry_t *b)
{
  if(a->sha1[0] && b->sha1[0] && strcmp(a->sha1, b->sha1) == 0)
    return TRUE;

  if(a->has_hash && b->has_hash)
  {
    const int dist = _hamming_distance(a->dhash, b->dhash);
    if(dist <= HAMMING_THRESHOLD)
      return TRUE;
  }

  if(a->datetime[0] && b->datetime[0]
     && a->maker[0] && b->maker[0]
     && strcmp(a->maker, b->maker) == 0
     && strcmp(a->model, b->model) == 0)
  {
    int y1, m1, d1, h1, min1, s1;
    int y2, m2, d2, h2, min2, s2;
    if(sscanf(a->datetime, "%d:%d:%d %d:%d:%d", &y1, &m1, &d1, &h1, &min1, &s1) == 6
       && sscanf(b->datetime, "%d:%d:%d %d:%d:%d", &y2, &m2, &d2, &h2, &min2, &s2) == 6)
    {
      const long ts1 = ((long)y1 * 365 * 24 + m1 * 30 * 24 + d1 * 24 + h1) * 3600 + min1 * 60 + s1;
      const long ts2 = ((long)y2 * 365 * 24 + m2 * 30 * 24 + d2 * 24 + h2) * 3600 + min2 * 60 + s2;
      if(labs(ts1 - ts2) <= TIMESTAMP_THRESHOLD_SEC
         && abs(a->width - b->width) <= 10
         && abs(a->height - b->height) <= 10)
        return TRUE;
    }
  }

  return FALSE;
}

static int _find_group(int *group_ids, int idx)
{
  while(group_ids[idx] != idx)
    idx = group_ids[idx];
  return idx;
}

static void _union_groups(int *group_ids, int a, int b)
{
  const int ra = _find_group(group_ids, a);
  const int rb = _find_group(group_ids, b);
  if(ra != rb)
    group_ids[ra] = rb;
}

static GList *_find_duplicate_groups(_dhash_entry_t *entries, int count)
{
  int *group_ids = malloc(count * sizeof(int));
  for(int i = 0; i < count; i++)
    group_ids[i] = i;

  for(int i = 0; i < count; i++)
  {
    for(int j = i + 1; j < count; j++)
    {
      if(_are_potential_duplicates(&entries[i], &entries[j]))
        _union_groups(group_ids, i, j);
    }
  }

  GHashTable *group_map = g_hash_table_new(g_direct_hash, g_direct_equal);
  for(int i = 0; i < count; i++)
  {
    const int root = _find_group(group_ids, i);
    GList *members = g_hash_table_lookup(group_map, GINT_TO_POINTER(root));
    members = g_list_append(members, &entries[i]);
    g_hash_table_insert(group_map, GINT_TO_POINTER(root), members);
  }

  GList *result = NULL;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, group_map);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    GList *members = (GList *)value;
    if(g_list_length(members) >= 2)
    {
      _dup_group_t *group = malloc(sizeof(_dup_group_t));
      group->entries = members;
      result = g_list_append(result, group);
    }
    else
    {
      g_list_free(members);
    }
  }

  g_hash_table_destroy(group_map);
  free(group_ids);

  return result;
}

static void _update_status_label(_dup_dialog_data_t *dd)
{
  int selected = 0;
  int total = 0;
  for(GList *l = dd->check_buttons; l; l = g_list_next(l))
  {
    _check_entry_t *ce = l->data;
    total++;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ce->check)))
      selected++;
  }
  char buf[256];
  snprintf(buf, sizeof(buf), _("%d images selected for deletion out of %d total duplicates"), selected, total);
  gtk_label_set_text(GTK_LABEL(dd->status_label), buf);
}

static void _on_check_toggled(GtkToggleButton *toggle, gpointer user_data)
{
  _dup_dialog_data_t *dd = (_dup_dialog_data_t *)user_data;
  _update_status_label(dd);
}

static void _select_all_duplicates_clicked(GtkButton *button, gpointer user_data)
{
  _dup_dialog_data_t *dd = (_dup_dialog_data_t *)user_data;
  for(GList *l = dd->check_buttons; l; l = g_list_next(l))
  {
    _check_entry_t *ce = l->data;
    if(!ce->is_keeper)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ce->check), TRUE);
  }
  _update_status_label(dd);
}

static void _deselect_all_clicked(GtkButton *button, gpointer user_data)
{
  _dup_dialog_data_t *dd = (_dup_dialog_data_t *)user_data;
  for(GList *l = dd->check_buttons; l; l = g_list_next(l))
  {
    _check_entry_t *ce = l->data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ce->check), FALSE);
  }
  _update_status_label(dd);
}

static void _delete_selected_clicked(GtkButton *button, gpointer user_data)
{
  _dup_dialog_data_t *dd = (_dup_dialog_data_t *)user_data;

  GList *to_delete = NULL;
  int count = 0;
  for(GList *l = dd->check_buttons; l; l = g_list_next(l))
  {
    _check_entry_t *ce = l->data;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ce->check)))
    {
      to_delete = g_list_append(to_delete, GINT_TO_POINTER(ce->imgid));
      count++;
    }
  }

  if(count == 0)
  {
    dt_control_log(_("no images selected for deletion"));
    return;
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *confirm = gtk_dialog_new_with_buttons(
    _("confirm deletion"),
    GTK_WINDOW(win),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_cancel"), GTK_RESPONSE_CANCEL,
    _("_remove from library"), GTK_RESPONSE_ACCEPT,
    _("_delete from disk"), GTK_RESPONSE_YES,
    NULL);

  char msg[512];
  snprintf(msg, sizeof(msg),
           _("are you sure you want to delete %d selected duplicate(s)?\n\n"
             "choose 'remove from library' to only remove from darktable,\n"
             "or 'delete from disk' to permanently delete the files."),
           count);

  GtkWidget *label = gtk_label_new(msg);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_margin_start(label, 12);
  gtk_widget_set_margin_end(label, 12);
  gtk_widget_set_margin_top(label, 12);
  gtk_widget_set_margin_bottom(label, 12);
  gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(confirm))), label);
  gtk_widget_show_all(confirm);

  const int response = gtk_dialog_run(GTK_DIALOG(confirm));
  gtk_widget_destroy(confirm);

  if(response == GTK_RESPONSE_ACCEPT || response == GTK_RESPONSE_YES)
  {
    const gboolean from_disk = (response == GTK_RESPONSE_YES);
    int deleted = 0;

    for(GList *l = to_delete; l; l = g_list_next(l))
    {
      const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
      if(from_disk)
      {
        char pathname[PATH_MAX] = { 0 };
        dt_image_full_path(imgid, pathname, sizeof(pathname), NULL);
        if(pathname[0])
        {
          g_unlink(pathname);
          char *c = pathname + strlen(pathname);
          while(c > pathname && *c != '.') c--;
          if(c > pathname)
          {
            g_strlcpy(c, ".xmp", pathname + sizeof(pathname) - c);
            g_unlink(pathname);
          }
        }
      }
      dt_image_remove(imgid);
      deleted++;
    }

    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_UNDEF, NULL);
    dt_control_queue_redraw_center();
    dt_control_log(ngettext("deleted %d duplicate image",
                            "deleted %d duplicate images", deleted), deleted);

    gtk_dialog_response(GTK_DIALOG(dd->dialog), GTK_RESPONSE_CLOSE);
  }

  g_list_free(to_delete);
}

static gboolean _draw_thumbnail(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const dt_imgid_t imgid = GPOINTER_TO_INT(user_data);

  const int w = gtk_widget_get_allocated_width(widget);
  const int h = gtk_widget_get_allocated_height(widget);

  cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
  cairo_paint(cr);

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_3, DT_MIPMAP_BEST_EFFORT, 'r');

  if(buf.buf && buf.width > 0 && buf.height > 0)
  {
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf.width);
    uint8_t *rgbbuf = malloc(stride * buf.height);

    for(int y = 0; y < buf.height; y++)
    {
      for(int x = 0; x < buf.width; x++)
      {
        const uint8_t *src = buf.buf + (y * buf.width + x) * 4;
        uint8_t *dst = rgbbuf + y * stride + x * 4;
        dst[0] = src[2]; // B
        dst[1] = src[1]; // G
        dst[2] = src[0]; // R
        dst[3] = 0;
      }
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
      rgbbuf, CAIRO_FORMAT_RGB24, buf.width, buf.height, stride);

    const double scale_x = (double)w / buf.width;
    const double scale_y = (double)h / buf.height;
    const double scale = fmin(scale_x, scale_y);
    const double off_x = (w - buf.width * scale) / 2.0;
    const double off_y = (h - buf.height * scale) / 2.0;

    cairo_save(cr);
    cairo_translate(cr, off_x, off_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(surface);
    free(rgbbuf);
  }
  else
  {
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, 5, h / 2.0);
    cairo_show_text(cr, _("no preview"));
  }

  dt_mipmap_cache_release(&buf);
  return TRUE;
}

static void _build_duplicate_dialog(GList *groups)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

  _dup_dialog_data_t *dd = calloc(1, sizeof(_dup_dialog_data_t));
  dd->groups = groups;
  dd->check_buttons = NULL;

  dd->dialog = gtk_dialog_new_with_buttons(
    _("duplicate images found"),
    GTK_WINDOW(win),
    GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_close"), GTK_RESPONSE_CLOSE,
    NULL);

  gtk_window_set_default_size(GTK_WINDOW(dd->dialog), 900, 650);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dd->dialog));

  dd->status_label = gtk_label_new("");
  gtk_widget_set_margin_start(dd->status_label, 8);
  gtk_widget_set_margin_top(dd->status_label, 4);
  gtk_widget_set_margin_bottom(dd->status_label, 4);
  gtk_label_set_xalign(GTK_LABEL(dd->status_label), 0.0f);
  gtk_container_add(GTK_CONTAINER(content), dd->status_label);

  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_start(toolbar, 8);
  gtk_widget_set_margin_end(toolbar, 8);
  gtk_widget_set_margin_bottom(toolbar, 4);

  GtkWidget *select_all_btn = gtk_button_new_with_label(_("select all duplicates"));
  gtk_widget_set_tooltip_text(select_all_btn,
    _("select all duplicate images for deletion, keeping one per group"));
  g_signal_connect(select_all_btn, "clicked", G_CALLBACK(_select_all_duplicates_clicked), dd);
  gtk_box_pack_start(GTK_BOX(toolbar), select_all_btn, FALSE, FALSE, 0);

  GtkWidget *deselect_btn = gtk_button_new_with_label(_("deselect all"));
  g_signal_connect(deselect_btn, "clicked", G_CALLBACK(_deselect_all_clicked), dd);
  gtk_box_pack_start(GTK_BOX(toolbar), deselect_btn, FALSE, FALSE, 0);

  GtkWidget *delete_btn = gtk_button_new_with_label(_("delete selected"));
  gtk_widget_set_tooltip_text(delete_btn,
    _("delete all checked images"));
  g_signal_connect(delete_btn, "clicked", G_CALLBACK(_delete_selected_clicked), dd);
  gtk_box_pack_end(GTK_BOX(toolbar), delete_btn, FALSE, FALSE, 0);

  gtk_container_add(GTK_CONTAINER(content), toolbar);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_container_add(GTK_CONTAINER(content), scrolled);

  dd->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(dd->content_box, 8);
  gtk_widget_set_margin_end(dd->content_box, 8);
  gtk_widget_set_margin_top(dd->content_box, 8);
  gtk_widget_set_margin_bottom(dd->content_box, 8);
  gtk_container_add(GTK_CONTAINER(scrolled), dd->content_box);

  int group_num = 0;
  for(GList *gl = groups; gl; gl = g_list_next(gl))
  {
    _dup_group_t *group = gl->data;
    group_num++;

    char group_label[128];
    snprintf(group_label, sizeof(group_label),
             _("duplicate group %d (%d images)"),
             group_num, g_list_length(group->entries));

    GtkWidget *frame = gtk_frame_new(group_label);
    gtk_box_pack_start(GTK_BOX(dd->content_box), frame, FALSE, FALSE, 0);

    GtkWidget *group_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(group_box, 6);
    gtk_widget_set_margin_end(group_box, 6);
    gtk_widget_set_margin_top(group_box, 6);
    gtk_widget_set_margin_bottom(group_box, 6);
    gtk_container_add(GTK_CONTAINER(frame), group_box);

    gboolean first = TRUE;
    for(GList *el = group->entries; el; el = g_list_next(el))
    {
      _dhash_entry_t *entry = el->data;

      GtkWidget *img_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_box_pack_start(GTK_BOX(group_box), img_box, FALSE, FALSE, 2);

      GtkWidget *drawing = gtk_drawing_area_new();
      gtk_widget_set_size_request(drawing, 160, 120);
      g_signal_connect(drawing, "draw", G_CALLBACK(_draw_thumbnail),
                       GINT_TO_POINTER(entry->imgid));
      gtk_box_pack_start(GTK_BOX(img_box), drawing, FALSE, FALSE, 0);

      const char *basename = strrchr(entry->filename, '/');
      if(!basename) basename = entry->filename;
      else basename++;

      GtkWidget *name_label = gtk_label_new(basename);
      gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_MIDDLE);
      gtk_label_set_max_width_chars(GTK_LABEL(name_label), 22);
      gtk_box_pack_start(GTK_BOX(img_box), name_label, FALSE, FALSE, 0);

      char info[128];
      snprintf(info, sizeof(info), "%dx%d", entry->width, entry->height);
      GtkWidget *info_label = gtk_label_new(info);
      gtk_widget_set_opacity(info_label, 0.7);
      gtk_box_pack_start(GTK_BOX(img_box), info_label, FALSE, FALSE, 0);

      _check_entry_t *ce = malloc(sizeof(_check_entry_t));
      ce->imgid = entry->imgid;
      ce->is_keeper = first;

      if(first)
      {
        ce->check = gtk_check_button_new_with_label(_("keep (original)"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ce->check), FALSE);
        gtk_widget_set_sensitive(ce->check, FALSE);
        first = FALSE;
      }
      else
      {
        ce->check = gtk_check_button_new_with_label(_("delete"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ce->check), FALSE);
        g_signal_connect(ce->check, "toggled", G_CALLBACK(_on_check_toggled), dd);
      }
      gtk_box_pack_start(GTK_BOX(img_box), ce->check, FALSE, FALSE, 0);

      dd->check_buttons = g_list_append(dd->check_buttons, ce);
    }
  }

  _update_status_label(dd);

  gtk_widget_show_all(dd->dialog);
  gtk_dialog_run(GTK_DIALOG(dd->dialog));
  gtk_widget_destroy(dd->dialog);

  for(GList *l = dd->check_buttons; l; l = g_list_next(l))
    free(l->data);
  g_list_free(dd->check_buttons);

  for(GList *l = groups; l; l = g_list_next(l))
  {
    _dup_group_t *g_dup = l->data;
    g_list_free(g_dup->entries);
    free(g_dup);
  }
  g_list_free(groups);

  free(dd);
}

static void _detect_duplicates_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_control_log(_("scanning for duplicate images..."));

  GList *all_images = dt_collection_get_all(darktable.collection, -1);
  const int count = g_list_length(all_images);

  if(count < 2)
  {
    dt_control_log(_("need at least 2 images to detect duplicates"));
    g_list_free(all_images);
    return;
  }

  _dhash_entry_t *entries = calloc(count, sizeof(_dhash_entry_t));

  int idx = 0;
  for(GList *l = all_images; l; l = g_list_next(l))
  {
    entries[idx].imgid = GPOINTER_TO_INT(l->data);
    _get_image_info(&entries[idx]);
    _compute_image_dhash(&entries[idx]);
    idx++;
  }
  g_list_free(all_images);

  GList *groups = _find_duplicate_groups(entries, count);

  if(!groups)
  {
    dt_control_log(_("no duplicate images found"));
    free(entries);
    return;
  }

  const int num_groups = g_list_length(groups);
  int total_dups = 0;
  for(GList *gl = groups; gl; gl = g_list_next(gl))
  {
    _dup_group_t *g_dup = gl->data;
    total_dups += g_list_length(g_dup->entries);
  }

  dt_control_log(ngettext("found %d group with %d duplicate images",
                           "found %d groups with %d duplicate images", num_groups),
                 num_groups, total_dups);

  _build_duplicate_dialog(groups);

  free(entries);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_duplicate_detect_t *d = calloc(1, sizeof(dt_lib_duplicate_detect_t));
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  d->detect_button = dt_action_button_new(
    self, N_("detect duplicates"), _detect_duplicates_clicked, NULL,
    _("scan the current collection for duplicate\n"
      "or visually similar images"),
    0, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->detect_button, TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
