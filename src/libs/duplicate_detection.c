/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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
#include "common/database.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "dtgtk/thumbnail.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <math.h>
#include <string.h>

DT_MODULE(1)

#define _DD_TIME_WINDOW_US (15 * 60 * G_TIME_SPAN_SECOND)

typedef struct _dd_entry_t
{
  dt_imgid_t imgid;
  dt_filmid_t film_id;
  char filename[DT_MAX_FILENAME_LEN];
  char *sha1;
  GTimeSpan taken;
  int width, height;
  gboolean has_sig;
  uint64_t ahash;
  uint64_t dhash;
  float mean_r, mean_g, mean_b, mean_l;
} _dd_entry_t;

typedef struct _dd_group_result_t
{
  GArray *imgids; // dt_imgid_t
} _dd_group_result_t;

typedef struct dt_lib_duplicate_detection_t dt_lib_duplicate_detection_t;

typedef struct _dd_ui_group_t _dd_ui_group_t;

typedef struct _dd_ui_item_t
{
  dt_imgid_t imgid;
  gboolean keep;
  gboolean deleted;
  GtkWidget *row;
  GtkWidget *check;
  GtkWidget *delete_button;
  dt_thumbnail_t *thumb;
  _dd_ui_group_t *group;
} _dd_ui_item_t;

struct _dd_ui_group_t
{
  dt_lib_duplicate_detection_t *owner;
  GPtrArray *items; // _dd_ui_item_t*
};

struct dt_lib_duplicate_detection_t
{
  GtkWidget *detect_button;
  GtkWidget *result_window;
  GtkWidget *summary_label;
  GPtrArray *groups; // _dd_ui_group_t*
};

const char *name(dt_lib_module_t *self)
{
  return _("duplicate detection");
}

const char *description(dt_lib_module_t *self)
{
  return _("detect exact and near-duplicate photos\n"
           "then review and delete duplicates");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 560;
}

static int _uf_find(int *parent, int idx)
{
  if(parent[idx] != idx) parent[idx] = _uf_find(parent, parent[idx]);
  return parent[idx];
}

static void _uf_union(int *parent, int *rank, int a, int b)
{
  int ra = _uf_find(parent, a);
  int rb = _uf_find(parent, b);
  if(ra == rb) return;
  if(rank[ra] < rank[rb])
    parent[ra] = rb;
  else if(rank[ra] > rank[rb])
    parent[rb] = ra;
  else
  {
    parent[rb] = ra;
    rank[ra]++;
  }
}

static inline float _luma_from_rgba(const uint8_t *px)
{
  return 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
}

static gboolean _compute_signature(const dt_imgid_t imgid, _dd_entry_t *entry)
{
  dt_mipmap_buffer_t buf = { 0 };
  dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_2, DT_MIPMAP_BEST_EFFORT, 'r');
  if(!buf.buf || buf.width < 9 || buf.height < 8)
  {
    if(buf.buf) dt_mipmap_cache_release(&buf);
    return FALSE;
  }

  float sum_l = 0.0f;
  float sum_r = 0.0f;
  float sum_g = 0.0f;
  float sum_b = 0.0f;
  float lum8x8[64] = { 0.0f };
  float lum9x8[72] = { 0.0f };
  int p8 = 0;
  int p9 = 0;

  for(int y = 0; y < 8; y++)
  {
    const int sy = CLAMP((int)((y + 0.5f) * (buf.height / 8.0f)), 0, buf.height - 1);
    for(int x = 0; x < 8; x++)
    {
      const int sx = CLAMP((int)((x + 0.5f) * (buf.width / 8.0f)), 0, buf.width - 1);
      const uint8_t *px = buf.buf + ((size_t)sy * buf.width + sx) * 4;
      const float l = _luma_from_rgba(px);
      lum8x8[p8++] = l;
      sum_l += l;
      sum_r += px[0];
      sum_g += px[1];
      sum_b += px[2];
    }
    for(int x = 0; x < 9; x++)
    {
      const int sx = CLAMP((int)((x + 0.5f) * (buf.width / 9.0f)), 0, buf.width - 1);
      const uint8_t *px = buf.buf + ((size_t)sy * buf.width + sx) * 4;
      lum9x8[p9++] = _luma_from_rgba(px);
    }
  }

  const float avg_l = sum_l / 64.0f;
  uint64_t ahash = 0;
  uint64_t dhash = 0;
  for(int i = 0; i < 64; i++)
  {
    if(lum8x8[i] >= avg_l) ahash |= (1ULL << i);
  }

  int bit = 0;
  for(int y = 0; y < 8; y++)
  {
    for(int x = 0; x < 8; x++, bit++)
    {
      const float l = lum9x8[y * 9 + x];
      const float r = lum9x8[y * 9 + x + 1];
      if(l >= r) dhash |= (1ULL << bit);
    }
  }

  entry->ahash = ahash;
  entry->dhash = dhash;
  entry->mean_l = avg_l;
  entry->mean_r = sum_r / 64.0f;
  entry->mean_g = sum_g / 64.0f;
  entry->mean_b = sum_b / 64.0f;
  entry->has_sig = TRUE;
  entry->width = buf.width;
  entry->height = buf.height;

  dt_mipmap_cache_release(&buf);
  return TRUE;
}

static gboolean _are_visually_similar(const _dd_entry_t *a, const _dd_entry_t *b)
{
  if(!a->has_sig || !b->has_sig) return FALSE;
  if(a->width <= 0 || a->height <= 0 || b->width <= 0 || b->height <= 0) return FALSE;

  const float ar_a = (float)a->width / a->height;
  const float ar_b = (float)b->width / b->height;
  if(fabsf(ar_a - ar_b) > 0.18f) return FALSE;

  const int ham_a = __builtin_popcountll(a->ahash ^ b->ahash);
  const int ham_d = __builtin_popcountll(a->dhash ^ b->dhash);
  const float color_delta = fabsf(a->mean_r - b->mean_r) + fabsf(a->mean_g - b->mean_g)
                            + fabsf(a->mean_b - b->mean_b);
  const float luma_delta = fabsf(a->mean_l - b->mean_l);

  if(ham_a <= 2 && ham_d <= 3 && color_delta <= 25.0f && luma_delta <= 12.0f) return TRUE;
  if(ham_a <= 8 && ham_d <= 10 && color_delta <= 80.0f && luma_delta <= 35.0f) return TRUE;

  return FALSE;
}

static void _group_result_free(gpointer data)
{
  _dd_group_result_t *group = (_dd_group_result_t *)data;
  if(group->imgids) g_array_unref(group->imgids);
  g_free(group);
}

static void _entry_cleanup(_dd_entry_t *entries, const int n)
{
  for(int i = 0; i < n; i++)
    g_free(entries[i].sha1);
  g_free(entries);
}

static GPtrArray *_build_duplicate_groups(int *out_count)
{
  GList *imgs = dt_collection_get_all(darktable.collection, -1);
  const int n = g_list_length(imgs);
  if(out_count) *out_count = n;

  GPtrArray *groups = g_ptr_array_new_with_free_func(_group_result_free);
  if(n < 2)
  {
    g_list_free(imgs);
    return groups;
  }

  _dd_entry_t *entries = g_malloc0_n(n, sizeof(_dd_entry_t));
  int *parent = g_malloc_n(n, sizeof(int));
  int *rank = g_malloc0_n(n, sizeof(int));

  sqlite3_stmt *sha_stmt = NULL;
  sqlite3_prepare_v2(dt_database_get(darktable.db), "SELECT sha1sum FROM main.images WHERE id = ?1", -1, &sha_stmt,
                     NULL);

  int i = 0;
  for(GList *l = imgs; l; l = g_list_next(l), i++)
  {
    const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
    entries[i].imgid = imgid;
    parent[i] = i;

    const dt_image_t *img = dt_image_cache_get(imgid, 'r');
    if(img)
    {
      entries[i].film_id = img->film_id;
      g_strlcpy(entries[i].filename, img->filename, sizeof(entries[i].filename));
      entries[i].taken = img->exif_datetime_taken;
      entries[i].width = img->width;
      entries[i].height = img->height;
      dt_image_cache_read_release(img);
    }

    if(sha_stmt)
    {
      DT_DEBUG_SQLITE3_BIND_INT(sha_stmt, 1, imgid);
      if(sqlite3_step(sha_stmt) == SQLITE_ROW)
      {
        const unsigned char *txt = sqlite3_column_text(sha_stmt, 0);
        if(txt && txt[0]) entries[i].sha1 = g_strdup((const char *)txt);
      }
      DT_DEBUG_SQLITE3_RESET(sha_stmt);
      DT_DEBUG_SQLITE3_CLEAR_BINDINGS(sha_stmt);
    }

    _compute_signature(imgid, &entries[i]);
  }
  if(sha_stmt) sqlite3_finalize(sha_stmt);
  g_list_free(imgs);

  GHashTable *by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
  GHashTable *by_sha1 = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

  for(i = 0; i < n; i++)
  {
    if(entries[i].film_id > 0 && entries[i].filename[0])
    {
      gchar *key = g_strdup_printf("%u::%s", entries[i].film_id, entries[i].filename);
      GPtrArray *arr = g_hash_table_lookup(by_name, key);
      if(!arr)
      {
        arr = g_ptr_array_new();
        g_hash_table_insert(by_name, key, arr);
      }
      else
        g_free(key);
      g_ptr_array_add(arr, GINT_TO_POINTER(i));
    }

    if(entries[i].sha1 && entries[i].sha1[0])
    {
      GPtrArray *arr = g_hash_table_lookup(by_sha1, entries[i].sha1);
      if(!arr)
      {
        arr = g_ptr_array_new();
        g_hash_table_insert(by_sha1, g_strdup(entries[i].sha1), arr);
      }
      g_ptr_array_add(arr, GINT_TO_POINTER(i));
    }
  }

  GHashTableIter iter;
  gpointer k = NULL;
  gpointer v = NULL;
  g_hash_table_iter_init(&iter, by_name);
  while(g_hash_table_iter_next(&iter, &k, &v))
  {
    GPtrArray *arr = (GPtrArray *)v;
    if(arr->len < 2) continue;
    const int root = GPOINTER_TO_INT(g_ptr_array_index(arr, 0));
    for(guint j = 1; j < arr->len; j++)
      _uf_union(parent, rank, root, GPOINTER_TO_INT(g_ptr_array_index(arr, j)));
  }

  g_hash_table_iter_init(&iter, by_sha1);
  while(g_hash_table_iter_next(&iter, &k, &v))
  {
    GPtrArray *arr = (GPtrArray *)v;
    if(arr->len < 2) continue;
    const int root = GPOINTER_TO_INT(g_ptr_array_index(arr, 0));
    for(guint j = 1; j < arr->len; j++)
      _uf_union(parent, rank, root, GPOINTER_TO_INT(g_ptr_array_index(arr, j)));
  }

  for(i = 0; i < n; i++)
  {
    for(int j = i + 1; j < n; j++)
    {
      if(!entries[i].has_sig || !entries[j].has_sig) continue;

      if(entries[i].taken > 0 && entries[j].taken > 0)
      {
        const GTimeSpan dt = llabs(entries[i].taken - entries[j].taken);
        if(dt > _DD_TIME_WINDOW_US)
        {
          // For images far apart in time, require a stronger visual match.
          const int ham_a = __builtin_popcountll(entries[i].ahash ^ entries[j].ahash);
          const int ham_d = __builtin_popcountll(entries[i].dhash ^ entries[j].dhash);
          if(ham_a > 3 || ham_d > 4) continue;
        }
      }

      if(_are_visually_similar(&entries[i], &entries[j]))
        _uf_union(parent, rank, i, j);
    }
  }

  GHashTable *components
      = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_ptr_array_unref);
  for(i = 0; i < n; i++)
  {
    const int root = _uf_find(parent, i);
    GPtrArray *arr = g_hash_table_lookup(components, GINT_TO_POINTER(root));
    if(!arr)
    {
      arr = g_ptr_array_new();
      g_hash_table_insert(components, GINT_TO_POINTER(root), arr);
    }
    g_ptr_array_add(arr, GINT_TO_POINTER(i));
  }

  g_hash_table_iter_init(&iter, components);
  while(g_hash_table_iter_next(&iter, &k, &v))
  {
    GPtrArray *idx = (GPtrArray *)v;
    if(idx->len < 2) continue;
    _dd_group_result_t *group = g_malloc0(sizeof(_dd_group_result_t));
    group->imgids = g_array_sized_new(FALSE, FALSE, sizeof(dt_imgid_t), idx->len);
    for(guint p = 0; p < idx->len; p++)
    {
      const int row = GPOINTER_TO_INT(g_ptr_array_index(idx, p));
      const dt_imgid_t imgid = entries[row].imgid;
      g_array_append_val(group->imgids, imgid);
    }
    g_ptr_array_add(groups, group);
  }

  g_hash_table_destroy(components);
  g_hash_table_destroy(by_name);
  g_hash_table_destroy(by_sha1);
  g_free(parent);
  g_free(rank);
  _entry_cleanup(entries, n);

  return groups;
}

static void _ui_item_free(gpointer data)
{
  _dd_ui_item_t *item = (_dd_ui_item_t *)data;
  if(item->thumb)
  {
    // The result window owns thumbnail widgets and destroys them first.
    // Avoid a second gtk_widget_destroy() from dt_thumbnail_destroy().
    item->thumb->w_main = NULL;
    dt_thumbnail_destroy(item->thumb);
  }
  g_free(item);
}

static void _ui_group_free(gpointer data)
{
  _dd_ui_group_t *group = (_dd_ui_group_t *)data;
  if(group->items) g_ptr_array_free(group->items, TRUE);
  g_free(group);
}

static void _clear_results_data(dt_lib_duplicate_detection_t *d)
{
  if(!d->groups) return;
  g_ptr_array_free(d->groups, TRUE);
  d->groups = NULL;
  d->summary_label = NULL;
}

static void _update_summary(dt_lib_duplicate_detection_t *d)
{
  if(!d || !d->summary_label || !d->groups) return;

  int pending = 0;
  int checked = 0;
  for(guint g = 0; g < d->groups->len; g++)
  {
    _dd_ui_group_t *group = g_ptr_array_index(d->groups, g);
    for(guint i = 0; i < group->items->len; i++)
    {
      _dd_ui_item_t *item = g_ptr_array_index(group->items, i);
      if(item->keep || item->deleted) continue;
      pending++;
      if(item->check && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(item->check))) checked++;
    }
  }

  gchar *txt = g_strdup_printf(_("groups: %d — remaining duplicates: %d — selected for bulk delete: %d"),
                               d->groups->len, pending, checked);
  gtk_label_set_text(GTK_LABEL(d->summary_label), txt);
  g_free(txt);
}

static void _mark_item_deleted(_dd_ui_item_t *item)
{
  if(!item || item->deleted || item->keep) return;
  item->deleted = TRUE;
  if(item->check)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item->check), FALSE);
    gtk_widget_set_sensitive(item->check, FALSE);
  }
  if(item->delete_button) gtk_widget_set_sensitive(item->delete_button, FALSE);
  if(item->row) gtk_widget_set_opacity(item->row, 0.45);
}

static void _delete_item(_dd_ui_item_t *item)
{
  if(!item || item->keep || item->deleted) return;
  dt_control_delete_duplicate(item->imgid);
  _mark_item_deleted(item);
}

static void _delete_single_clicked(GtkButton *button, gpointer user_data)
{
  _dd_ui_item_t *item = (_dd_ui_item_t *)user_data;
  _delete_item(item);
  if(item && item->group)
  {
    _update_summary(item->group->owner);
  }
}

static void _check_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_lib_duplicate_detection_t *d = (dt_lib_duplicate_detection_t *)user_data;
  _update_summary(d);
}

static int _delete_checked_group(_dd_ui_group_t *group)
{
  int cnt = 0;
  for(guint i = 0; i < group->items->len; i++)
  {
    _dd_ui_item_t *item = g_ptr_array_index(group->items, i);
    if(item->keep || item->deleted || !item->check) continue;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(item->check)))
    {
      _delete_item(item);
      cnt++;
    }
  }
  return cnt;
}

static void _delete_group_clicked(GtkButton *button, gpointer user_data)
{
  _dd_ui_group_t *group = (_dd_ui_group_t *)user_data;
  dt_lib_duplicate_detection_t *d = group->owner;
  const int cnt = _delete_checked_group(group);
  if(cnt <= 0)
    dt_control_log(_("no duplicates selected in this group"));
  else
    dt_control_log(ngettext("queued %d duplicate for deletion", "queued %d duplicates for deletion", cnt), cnt);
  _update_summary(d);
}

static void _delete_selected_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_duplicate_detection_t *d = (dt_lib_duplicate_detection_t *)user_data;
  if(!d || !d->groups) return;

  int total = 0;
  for(guint g = 0; g < d->groups->len; g++)
  {
    _dd_ui_group_t *group = g_ptr_array_index(d->groups, g);
    total += _delete_checked_group(group);
  }

  if(total <= 0)
    dt_control_log(_("no duplicates selected for deletion"));
  else
    dt_control_log(ngettext("queued %d duplicate for deletion", "queued %d duplicates for deletion", total), total);

  _update_summary(d);
}

static void _results_window_destroy(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_duplicate_detection_t *d = self->data;
  d->result_window = NULL;
  _clear_results_data(d);
}

static void _populate_results_window(dt_lib_module_t *self, GPtrArray *groups)
{
  if(!self || !self->data || !groups) return;

  dt_lib_duplicate_detection_t *d = self->data;

  if(d->result_window)
  {
    gtk_widget_destroy(d->result_window);
    d->result_window = NULL;
  }
  _clear_results_data(d);
  d->groups = g_ptr_array_new_with_free_func(_ui_group_free);

  d->result_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(d->result_window), _("duplicate detection results"));
  gtk_window_set_transient_for(GTK_WINDOW(d->result_window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(d->result_window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(d->result_window), 1024, 720);
  g_signal_connect(G_OBJECT(d->result_window), "destroy", G_CALLBACK(_results_window_destroy), self);

  GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(main_box), 12);
  gtk_container_add(GTK_CONTAINER(d->result_window), main_box);

  d->summary_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->summary_label), 0.0f);
  dt_gui_add_class(d->summary_label, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(main_box), d->summary_label, FALSE, FALSE, 0);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(main_box), scrolled, TRUE, TRUE, 0);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_add(GTK_CONTAINER(scrolled), content);

  for(guint g = 0; g < groups->len; g++)
  {
    _dd_group_result_t *gr = g_ptr_array_index(groups, g);
    if(!gr->imgids || gr->imgids->len < 2) continue;

    _dd_ui_group_t *uig = g_malloc0(sizeof(_dd_ui_group_t));
    uig->owner = d;
    uig->items = g_ptr_array_new_with_free_func(_ui_item_free);
    g_ptr_array_add(d->groups, uig);

    gchar *title = g_strdup_printf(_("group %d — %d photos"), g + 1, gr->imgids->len);
    GtkWidget *frame = gtk_frame_new(title);
    g_free(title);
    gtk_box_pack_start(GTK_BOX(content), frame, FALSE, FALSE, 0);

    GtkWidget *frame_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(frame_box), 8);
    gtk_container_add(GTK_CONTAINER(frame), frame_box);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_box_pack_start(GTK_BOX(frame_box), flow, FALSE, FALSE, 0);

    for(guint i = 0; i < gr->imgids->len; i++)
    {
      const dt_imgid_t imgid = g_array_index(gr->imgids, dt_imgid_t, i);
      const gboolean keep = (i == 0);

      _dd_ui_item_t *item = g_malloc0(sizeof(_dd_ui_item_t));
      item->imgid = imgid;
      item->keep = keep;
      item->group = uig;
      g_ptr_array_add(uig->items, item);

      GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      item->row = card;
      gtk_widget_set_hexpand(card, TRUE);

      item->thumb = dt_thumbnail_new(130,
                                     130,
                                     IMG_TO_FIT,
                                     imgid,
                                     -1,
                                     DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL,
                                     DT_THUMBNAIL_CONTAINER_DUPLICATE,
                                     TRUE,
                                     DT_THUMBNAIL_SELECTION_UNSELECTED);
      if(item->thumb && item->thumb->w_main)
      {
        item->thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_DISABLED;
        item->thumb->disable_actions = TRUE;
        item->thumb->disable_mouseover = TRUE;
        gtk_box_pack_start(GTK_BOX(card), item->thumb->w_main, FALSE, FALSE, 0);
      }
      else
      {
        if(item->thumb)
        {
          dt_thumbnail_destroy(item->thumb);
          item->thumb = NULL;
        }
        GtkWidget *missing_thumb = gtk_label_new(_("thumbnail unavailable"));
        gtk_label_set_xalign(GTK_LABEL(missing_thumb), 0.0f);
        gtk_box_pack_start(GTK_BOX(card), missing_thumb, FALSE, FALSE, 0);
      }

      GtkWidget *status = gtk_label_new(keep ? _("keep (reference)") : _("candidate duplicate"));
      gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
      gtk_box_pack_start(GTK_BOX(card), status, FALSE, FALSE, 0);

      if(!keep)
      {
        item->check = gtk_check_button_new_with_label(_("select for bulk delete"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item->check), TRUE);
        g_signal_connect(G_OBJECT(item->check), "toggled", G_CALLBACK(_check_toggled), d);
        gtk_box_pack_start(GTK_BOX(card), item->check, FALSE, FALSE, 0);

        item->delete_button = gtk_button_new_with_label(_("delete photo"));
        g_signal_connect(G_OBJECT(item->delete_button), "clicked", G_CALLBACK(_delete_single_clicked), item);
        gtk_box_pack_start(GTK_BOX(card), item->delete_button, FALSE, FALSE, 0);
      }

      gtk_container_add(GTK_CONTAINER(flow), card);
    }

    GtkWidget *group_delete = gtk_button_new_with_label(_("delete selected duplicates in this group"));
    g_signal_connect(G_OBJECT(group_delete), "clicked", G_CALLBACK(_delete_group_clicked), uig);
    gtk_box_pack_start(GTK_BOX(frame_box), group_delete, FALSE, FALSE, 0);
  }

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(main_box), buttons, FALSE, FALSE, 0);

  GtkWidget *delete_selected = gtk_button_new_with_label(_("Delete Selected Duplicates"));
  g_signal_connect(G_OBJECT(delete_selected), "clicked", G_CALLBACK(_delete_selected_clicked), d);
  gtk_box_pack_start(GTK_BOX(buttons), delete_selected, FALSE, FALSE, 0);

  GtkWidget *close_btn = gtk_button_new_with_label(_("close"));
  g_signal_connect_swapped(G_OBJECT(close_btn), "clicked", G_CALLBACK(gtk_widget_destroy), d->result_window);
  gtk_box_pack_end(GTK_BOX(buttons), close_btn, FALSE, FALSE, 0);

  _update_summary(d);
  gtk_widget_show_all(d->result_window);
}

static void _detect_duplicates_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(!self || !self->data) return;
  dt_lib_duplicate_detection_t *d = self->data;
  if(!d->detect_button) return;
  gtk_widget_set_sensitive(d->detect_button, FALSE);

  int scanned = 0;
  GPtrArray *groups = _build_duplicate_groups(&scanned);
  if(!groups)
  {
    gtk_widget_set_sensitive(d->detect_button, TRUE);
    return;
  }

  if(groups->len == 0)
  {
    dt_control_log(_("no duplicates detected in the current collection"));
    g_ptr_array_free(groups, TRUE);
  }
  else
  {
    _populate_results_window(self, groups);
    dt_control_log(ngettext("detected %d duplicate group from %d photo",
                            "detected %d duplicate groups from %d photos",
                            groups->len),
                   groups->len,
                   scanned);
    g_ptr_array_free(groups, TRUE);
  }

  gtk_widget_set_sensitive(d->detect_button, TRUE);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_duplicate_detection_t *d = self->data = g_malloc0(sizeof(dt_lib_duplicate_detection_t));

  d->detect_button = gtk_button_new_with_label(_("Detect Duplicates"));
  gtk_widget_set_tooltip_text(d->detect_button,
                              _("scan the current collection for exact and near-duplicate photos"));
  g_signal_connect(G_OBJECT(d->detect_button), "clicked", G_CALLBACK(_detect_duplicates_clicked), self);

  self->widget = dt_gui_vbox(d->detect_button);
  gtk_widget_show_all(self->widget);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_duplicate_detection_t *d = self->data;
  if(d->result_window) gtk_widget_destroy(d->result_window);
  _clear_results_data(d);
  g_free(d);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
