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

#include "libs/lib.h"

#include "common/curl_tools.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/introspection.h"
#include "common/pwstorage/pwstorage.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

#include <curl/curl.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

DT_MODULE(1)

#define DT_PHOTO_ASSISTANT_MODEL_KEY "plugins/darkroom/photo_assistant/model"
#define DT_PHOTO_ASSISTANT_SECRET_SLOT "photo_assistant_openai"
#define DT_PHOTO_ASSISTANT_SECRET_KEY "api_key"
#define DT_PHOTO_ASSISTANT_DEFAULT_MODEL "gpt-4.1-mini"
#define DT_PHOTO_ASSISTANT_REQUEST_TIMEOUT 90L

typedef struct dt_photo_assistant_apply_result_t
{
  gchar *summary;
  gchar *details;
  gint operations_applied;
} dt_photo_assistant_apply_result_t;

typedef struct dt_photo_assistant_request_t
{
  dt_lib_module_t *self;
  guint request_id;
  gchar *prompt;
  gchar *api_key;
  gchar *model;
  gchar *catalog;
} dt_photo_assistant_request_t;

typedef struct dt_photo_assistant_result_t
{
  dt_lib_module_t *self;
  guint request_id;
  gboolean success;
  gchar *status;
  gchar *plan_json;
} dt_photo_assistant_result_t;

typedef struct dt_lib_photo_assistant_t
{
  GtkWidget *api_key_entry;
  GtkWidget *model_entry;
  GtkWidget *prompt_view;
  GtkWidget *response_view;
  GtkWidget *status_label;
  GtkWidget *send_button;
  GtkWidget *save_key_button;
  gboolean busy;
  guint request_id;
} dt_lib_photo_assistant_t;

static gboolean _json_node_to_bool(JsonNode *node, gboolean *value);
static gboolean _json_node_to_double(JsonNode *node, gdouble *value);

static size_t _curl_write_string(void *ptr, size_t size, size_t nmemb, void *userdata)
{
  GString *response = (GString *)userdata;
  const size_t total = size * nmemb;
  if(total == 0) return 0;
  if(!ptr) return 0;
  g_string_append_len(response, (const gchar *)ptr, total);
  return total;
}

static gchar *_dup_text_buffer(GtkTextView *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void _set_text_buffer(GtkTextView *view, const gchar *text)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
  gtk_text_buffer_set_text(buffer, text ? text : "", -1);
}

static gchar *_collapse_ws_copy(const gchar *text)
{
  if(!text) return g_strdup("");

  gchar **parts = g_strsplit_set(text, "\r\n\t", -1);
  GString *out = g_string_new(NULL);
  for(gint i = 0; parts[i]; i++)
  {
    gchar *trimmed = g_strstrip(parts[i]);
    if(!trimmed[0]) continue;
    if(out->len) g_string_append_c(out, ' ');
    g_string_append(out, trimmed);
  }
  g_strfreev(parts);
  return g_string_free(out, FALSE);
}

static void _set_status(dt_lib_photo_assistant_t *d, const gchar *message)
{
  gtk_label_set_text(GTK_LABEL(d->status_label), message ? message : "");
}

static void _set_busy(dt_lib_photo_assistant_t *d, const gboolean busy)
{
  d->busy = busy;
  gtk_widget_set_sensitive(d->send_button, !busy);
  gtk_widget_set_sensitive(d->save_key_button, !busy);
}

static gboolean _field_is_supported(const dt_introspection_field_t *field)
{
  if(!field) return FALSE;

  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
    case DT_INTROSPECTION_TYPE_DOUBLE:
    case DT_INTROSPECTION_TYPE_INT8:
    case DT_INTROSPECTION_TYPE_UINT8:
    case DT_INTROSPECTION_TYPE_SHORT:
    case DT_INTROSPECTION_TYPE_USHORT:
    case DT_INTROSPECTION_TYPE_INT:
    case DT_INTROSPECTION_TYPE_UINT:
    case DT_INTROSPECTION_TYPE_LONG:
    case DT_INTROSPECTION_TYPE_ULONG:
    case DT_INTROSPECTION_TYPE_BOOL:
    case DT_INTROSPECTION_TYPE_ENUM:
      return field->header.name && *field->header.name;
    default:
      return FALSE;
  }
}

static gchar *_field_value_to_string(const dt_introspection_field_t *field, const void *base)
{
  if(!field || !base) return g_strdup("");

  const guint8 *ptr = (const guint8 *)base + field->header.offset;

  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
      return g_strdup_printf("%.4f", *(const float *)ptr);
    case DT_INTROSPECTION_TYPE_DOUBLE:
      return g_strdup_printf("%.4f", *(const double *)ptr);
    case DT_INTROSPECTION_TYPE_INT8:
      return g_strdup_printf("%d", *(const int8_t *)ptr);
    case DT_INTROSPECTION_TYPE_UINT8:
      return g_strdup_printf("%u", *(const uint8_t *)ptr);
    case DT_INTROSPECTION_TYPE_SHORT:
      return g_strdup_printf("%d", *(const short *)ptr);
    case DT_INTROSPECTION_TYPE_USHORT:
      return g_strdup_printf("%u", *(const unsigned short *)ptr);
    case DT_INTROSPECTION_TYPE_INT:
      return g_strdup_printf("%d", *(const int *)ptr);
    case DT_INTROSPECTION_TYPE_UINT:
      return g_strdup_printf("%u", *(const unsigned int *)ptr);
    case DT_INTROSPECTION_TYPE_LONG:
      return g_strdup_printf("%ld", *(const long *)ptr);
    case DT_INTROSPECTION_TYPE_ULONG:
      return g_strdup_printf("%lu", *(const unsigned long *)ptr);
    case DT_INTROSPECTION_TYPE_BOOL:
      return g_strdup(*(const gboolean *)ptr ? "true" : "false");
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      const int value = *(const int *)ptr;
      const char *name = dt_introspection_get_enum_name((dt_introspection_field_t *)field, value);
      return name ? g_strdup(name) : g_strdup_printf("%d", value);
    }
    default:
      return g_strdup("");
  }
}

static gchar *_field_limits_to_string(const dt_introspection_field_t *field)
{
  if(!field) return g_strdup("");

  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
      return g_strdup_printf("min=%.4f max=%.4f", field->Float.Min, field->Float.Max);
    case DT_INTROSPECTION_TYPE_DOUBLE:
      return g_strdup_printf("min=%.4f max=%.4f", field->Double.Min, field->Double.Max);
    case DT_INTROSPECTION_TYPE_INT8:
      return g_strdup_printf("min=%d max=%d", field->Int8.Min, field->Int8.Max);
    case DT_INTROSPECTION_TYPE_UINT8:
      return g_strdup_printf("min=%u max=%u", field->UInt8.Min, field->UInt8.Max);
    case DT_INTROSPECTION_TYPE_SHORT:
      return g_strdup_printf("min=%d max=%d", field->Short.Min, field->Short.Max);
    case DT_INTROSPECTION_TYPE_USHORT:
      return g_strdup_printf("min=%u max=%u", field->UShort.Min, field->UShort.Max);
    case DT_INTROSPECTION_TYPE_INT:
      return g_strdup_printf("min=%d max=%d", field->Int.Min, field->Int.Max);
    case DT_INTROSPECTION_TYPE_UINT:
      return g_strdup_printf("min=%u max=%u", field->UInt.Min, field->UInt.Max);
    case DT_INTROSPECTION_TYPE_LONG:
      return g_strdup_printf("min=%ld max=%ld", field->Long.Min, field->Long.Max);
    case DT_INTROSPECTION_TYPE_ULONG:
      return g_strdup_printf("min=%lu max=%lu", field->ULong.Min, field->ULong.Max);
    case DT_INTROSPECTION_TYPE_BOOL:
      return g_strdup("bool");
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      GString *values = g_string_new("values=");
      if(field->Enum.values)
      {
        gboolean first = TRUE;
        for(size_t i = 0; i < field->Enum.entries; i++)
        {
          const char *n = field->Enum.values[i].name;
          if(!n) break;
          if(!first) g_string_append_c(values, ',');
          first = FALSE;
          g_string_append(values, n);
        }
      }
      return g_string_free(values, FALSE);
    }
    default:
      return g_strdup("");
  }
}

static void _append_module_catalog_for_module(GString *catalog, dt_iop_module_t *module)
{
  if(!module
     || dt_iop_is_hidden(module)
     || (module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
     || !module->so
     || !module->so->get_introspection_linear)
    return;

  dt_introspection_field_t *fields = module->so->get_introspection_linear();
  if(!fields) return;

  const gchar *aliases = module->aliases ? module->aliases() : "";
  const char **description = module->description ? module->description(module) : NULL;
  gchar *collapsed_description = _collapse_ws_copy(description ? description[0] : "");

  gboolean appended_field = FALSE;
  GString *module_block = g_string_new(NULL);
  const gchar *op = module->op;
  const gchar *label = module->name() ? module->name() : "";
  g_string_append_printf(
    module_block,
    "module %s | label=%s | enabled=%s | aliases=%s | description=%s\n",
    op,
    label,
    module->enabled ? "true" : "false",
    aliases ? aliases : "",
    collapsed_description);

  for(dt_introspection_field_t *field = fields;
      field->header.type != DT_INTROSPECTION_TYPE_NONE;
      field++)
  {
    if(!_field_is_supported(field)) continue;

    gchar *current = _field_value_to_string(field, module->params);
    gchar *defaults = _field_value_to_string(field, module->default_params);
    gchar *limits = _field_limits_to_string(field);
    const gchar *desc = field->header.description && *field->header.description
                      ? field->header.description
                      : field->header.field_name;
    if(!desc) desc = "";
    const gchar *type_name = field->header.type_name ? field->header.type_name : "";

    g_string_append_printf(
      module_block,
      "  - %s | type=%s | current=%s | default=%s | %s | desc=%s\n",
      field->header.name,
      type_name,
      current,
      defaults,
      limits,
      desc);

    g_free(current);
    g_free(defaults);
    g_free(limits);
    appended_field = TRUE;
  }

  if(appended_field)
    g_string_append(catalog, module_block->str);

  g_string_free(module_block, TRUE);
  g_free(collapsed_description);
}

static gchar *_build_module_catalog(void)
{
  GString *catalog = g_string_new(
    "Only use module ops and field names exactly as listed here.\n"
    "Use absolute final values, not arithmetic expressions.\n"
    "Negative `hazeremoval.strength` adds haze instead of removing it.\n"
    "Lower `colorbalancergb.shadows_Y` darkens shadows.\n\n");

  for(GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
    _append_module_catalog_for_module(catalog, (dt_iop_module_t *)modules->data);

  return g_string_free(catalog, FALSE);
}

static gchar *_load_saved_api_key(void)
{
  GHashTable *table = dt_pwstorage_get(DT_PHOTO_ASSISTANT_SECRET_SLOT);
  gchar *key = NULL;

  if(table)
  {
    const gchar *value = g_hash_table_lookup(table, DT_PHOTO_ASSISTANT_SECRET_KEY);
    if(value && *value) key = g_strdup(value);
    g_hash_table_destroy(table);
  }

  return key;
}

static void _load_saved_api_key_into_entry(GtkWidget *entry)
{
  gchar *saved = _load_saved_api_key();
  if(saved && *saved)
    gtk_entry_set_text(GTK_ENTRY(entry), saved);
  g_free(saved);
}

static gboolean _save_api_key(const gchar *api_key)
{
  if(!api_key || !*api_key) return FALSE;

  GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert(table, g_strdup(DT_PHOTO_ASSISTANT_SECRET_KEY), g_strdup(api_key));
  const gboolean ok = dt_pwstorage_set(DT_PHOTO_ASSISTANT_SECRET_SLOT, table);
  g_hash_table_destroy(table);
  return ok;
}

static gchar *_json_node_dup_string(JsonNode *node)
{
  if(!node || !JSON_NODE_HOLDS_VALUE(node)) return NULL;

  const GType type = json_node_get_value_type(node);
  if(type == G_TYPE_STRING)
  {
    const gchar *text = json_node_get_string(node);
    return text ? g_strdup(text) : NULL;
  }
  if(type == G_TYPE_BOOLEAN)
    return g_strdup(json_node_get_boolean(node) ? "true" : "false");
  if(type == G_TYPE_INT64 || type == G_TYPE_DOUBLE)
    return g_strdup_printf("%.10g", json_node_get_double(node));

  return NULL;
}

static gchar *_json_node_to_compact_data(JsonNode *node)
{
  if(!node) return NULL;

  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, node);
#if JSON_CHECK_VERSION(0, 14, 0)
  json_generator_set_pretty(generator, FALSE);
#endif
  gchar *data = json_generator_to_data(generator, NULL);
  g_object_unref(generator);
  return data;
}

static gchar *_extract_openai_error(const gchar *payload)
{
  if(!payload || !*payload) return NULL;

  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, payload, -1, NULL))
  {
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  gchar *message = NULL;
  if(root && JSON_NODE_HOLDS_OBJECT(root))
  {
    JsonObject *object = json_node_get_object(root);
    if(json_object_has_member(object, "error"))
    {
      JsonObject *error = json_object_get_object_member(object, "error");
      if(json_object_has_member(error, "message"))
        message = g_strdup(json_object_get_string_member(error, "message"));
    }
  }

  g_object_unref(parser);
  return message;
}

static void _append_tool_schema_set_module_fields(JsonBuilder *builder)
{
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "function");
  json_builder_set_member_name(builder, "function");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "name");
  json_builder_add_string_value(builder, "set_module_fields");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(
    builder,
    "Apply darktable module changes by op name and field list. "
    "Use only module ops and field names present in the catalog.");
  json_builder_set_member_name(builder, "parameters");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "object");
  json_builder_set_member_name(builder, "properties");
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "module");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "string");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(builder, "darktable module op, for example vignette or hazeremoval");
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "enable");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "boolean");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(builder, "whether the module should be enabled");
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "focus");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "boolean");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(builder, "whether the module should receive darkroom focus after the edit");
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "reason");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "string");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(builder, "brief reason for this module change");
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "fields");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "array");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(builder, "field updates for this module");
  json_builder_set_member_name(builder, "items");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "object");
  json_builder_set_member_name(builder, "properties");
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "name");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "string");
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "value");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(builder, "absolute final value for the field");
  json_builder_end_object(builder);

  json_builder_end_object(builder);
  json_builder_set_member_name(builder, "required");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "name");
  json_builder_add_string_value(builder, "value");
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  json_builder_end_object(builder);

  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "required");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "module");
  json_builder_add_string_value(builder, "fields");
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  json_builder_end_object(builder);
  json_builder_end_object(builder);
  json_builder_end_object(builder);
}

static void _append_tool_schema_finish_edit_plan(JsonBuilder *builder)
{
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "function");
  json_builder_set_member_name(builder, "function");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "name");
  json_builder_add_string_value(builder, "finish_edit_plan");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(
    builder,
    "Finish the assistant run after all needed set_module_fields calls have been made.");
  json_builder_set_member_name(builder, "parameters");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "object");
  json_builder_set_member_name(builder, "properties");
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "summary");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "string");
  json_builder_set_member_name(builder, "description");
  json_builder_add_string_value(builder, "user-facing summary of the applied edits");
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "warnings");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "array");
  json_builder_set_member_name(builder, "items");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "string");
  json_builder_end_object(builder);
  json_builder_end_object(builder);

  json_builder_set_member_name(builder, "unhandled");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "array");
  json_builder_set_member_name(builder, "items");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "string");
  json_builder_end_object(builder);
  json_builder_end_object(builder);

  json_builder_end_object(builder);
  json_builder_set_member_name(builder, "required");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "summary");
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  json_builder_end_object(builder);
  json_builder_end_object(builder);
}

static gchar *_build_openai_tool_request_body(const gchar *model,
                                              const gchar *catalog,
                                              const gchar *prompt)
{
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "model");
  json_builder_add_string_value(builder, model && *model ? model : DT_PHOTO_ASSISTANT_DEFAULT_MODEL);

  json_builder_set_member_name(builder, "temperature");
  json_builder_add_double_value(builder, 0.2);

  json_builder_set_member_name(builder, "tool_choice");
  json_builder_add_string_value(builder, "auto");

  json_builder_set_member_name(builder, "messages");
  json_builder_begin_array(builder);

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "role");
  json_builder_add_string_value(builder, "system");
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(
    builder,
    "You are a darktable darkroom editing assistant. "
    "Use the provided tools to make actual module-linked edit decisions. "
    "Call set_module_fields once per module you want to change, using only module ops and field names present in the catalog. "
    "Use absolute final values, not deltas. "
    "When done, call finish_edit_plan exactly once with a concise summary plus any warnings or unhandled requests. "
    "Negative hazeremoval.strength adds haze, and lower colorbalancergb.shadows_Y darkens shadows.");
  json_builder_end_object(builder);

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "role");
  json_builder_add_string_value(builder, "user");
  json_builder_set_member_name(builder, "content");
  gchar *content = g_strdup_printf("User request:\n%s\n\nModule catalog:\n%s",
                                   prompt ? prompt : "",
                                   catalog ? catalog : "");
  json_builder_add_string_value(builder, content);
  g_free(content);
  json_builder_end_object(builder);

  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "tools");
  json_builder_begin_array(builder);
  _append_tool_schema_set_module_fields(builder);
  _append_tool_schema_finish_edit_plan(builder);
  json_builder_end_array(builder);

  json_builder_end_object(builder);

  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
#if JSON_CHECK_VERSION(0, 14, 0)
  json_generator_set_pretty(generator, FALSE);
#endif
  gchar *data = json_generator_to_data(generator, NULL);
  json_node_free(root);
  g_object_unref(generator);
  g_object_unref(builder);
  return data;
}

static gchar *_tool_arguments_to_operation_json(const gchar *arguments)
{
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, arguments ? arguments : "{}", -1, NULL))
  {
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *object = json_node_get_object(root);
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  if(json_object_has_member(object, "module"))
  {
    JsonNode *node = json_object_get_member(object, "module");
    gchar *value = _json_node_dup_string(node);
    if(value)
    {
      json_builder_set_member_name(builder, "module");
      json_builder_add_string_value(builder, value);
      g_free(value);
    }
  }

  if(json_object_has_member(object, "enable"))
  {
    gboolean enable = TRUE;
    if(_json_node_to_bool(json_object_get_member(object, "enable"), &enable))
    {
      json_builder_set_member_name(builder, "enable");
      json_builder_add_boolean_value(builder, enable);
    }
  }
  else
  {
    json_builder_set_member_name(builder, "enable");
    json_builder_add_boolean_value(builder, TRUE);
  }

  if(json_object_has_member(object, "focus"))
  {
    gboolean focus = FALSE;
    if(_json_node_to_bool(json_object_get_member(object, "focus"), &focus))
    {
      json_builder_set_member_name(builder, "focus");
      json_builder_add_boolean_value(builder, focus);
    }
  }

  if(json_object_has_member(object, "reason"))
  {
    gchar *reason = _json_node_dup_string(json_object_get_member(object, "reason"));
    if(reason)
    {
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, reason);
      g_free(reason);
    }
  }

  if(json_object_has_member(object, "fields"))
  {
    JsonNode *fields_node = json_object_get_member(object, "fields");
    if(fields_node && JSON_NODE_HOLDS_ARRAY(fields_node))
    {
      json_builder_set_member_name(builder, "fields");
      json_builder_begin_array(builder);
      JsonArray *fields = json_node_get_array(fields_node);
      const guint len = json_array_get_length(fields);
      for(guint i = 0; i < len; i++)
      {
        JsonNode *field_node = json_array_get_element(fields, i);
        if(!field_node || !JSON_NODE_HOLDS_OBJECT(field_node)) continue;

        JsonObject *field = json_node_get_object(field_node);
        json_builder_begin_object(builder);

        if(json_object_has_member(field, "name"))
        {
          gchar *name = _json_node_dup_string(json_object_get_member(field, "name"));
          if(name)
          {
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, name);
            g_free(name);
          }
        }

        if(json_object_has_member(field, "value"))
        {
          JsonNode *value_node = json_object_get_member(field, "value");
          gchar *value_data = _json_node_to_compact_data(value_node);
          if(value_data)
          {
            JsonParser *value_parser = json_parser_new();
            if(json_parser_load_from_data(value_parser, value_data, -1, NULL))
            {
              JsonNode *value_root = json_parser_get_root(value_parser);
              if(value_root)
              {
                json_builder_set_member_name(builder, "value");
                json_builder_add_value(builder, json_node_copy(value_root));
              }
            }
            g_object_unref(value_parser);
            g_free(value_data);
          }
        }

        json_builder_end_object(builder);
      }
      json_builder_end_array(builder);
    }
  }

  json_builder_end_object(builder);
  JsonGenerator *generator = json_generator_new();
  JsonNode *out_root = json_builder_get_root(builder);
  json_generator_set_root(generator, out_root);
  gchar *data = json_generator_to_data(generator, NULL);
  json_node_free(out_root);
  g_object_unref(generator);
  g_object_unref(builder);
  g_object_unref(parser);
  return data;
}

static gchar *_build_plan_json_from_tool_calls(JsonArray *tool_calls,
                                               gchar **error_message)
{
  if(error_message) *error_message = NULL;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "operations");
  json_builder_begin_array(builder);

  gchar *summary = NULL;
  GPtrArray *warnings = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *unhandled = g_ptr_array_new_with_free_func(g_free);
  gboolean saw_finish = FALSE;

  const guint len = json_array_get_length(tool_calls);
  for(guint i = 0; i < len; i++)
  {
    JsonObject *call = json_array_get_object_element(tool_calls, i);
    if(!call || !json_object_has_member(call, "type")) continue;
    const gchar *type = json_object_get_string_member(call, "type");
    if(!type || g_strcmp0(type, "function")) continue;
    if(!json_object_has_member(call, "function")) continue;

    JsonObject *function = json_object_get_object_member(call, "function");
    if(!function || !json_object_has_member(function, "name")) continue;

    const gchar *name = json_object_get_string_member(function, "name");
    const gchar *arguments = json_object_has_member(function, "arguments")
                           ? json_object_get_string_member(function, "arguments")
                           : "{}";

    if(g_strcmp0(name, "set_module_fields") == 0)
    {
      gchar *operation_json = _tool_arguments_to_operation_json(arguments);
      if(!operation_json) continue;

      JsonParser *operation_parser = json_parser_new();
      if(json_parser_load_from_data(operation_parser, operation_json, -1, NULL))
      {
        JsonNode *operation_root = json_parser_get_root(operation_parser);
        if(operation_root && JSON_NODE_HOLDS_OBJECT(operation_root))
          json_builder_add_value(builder, json_node_copy(operation_root));
      }
      g_object_unref(operation_parser);
      g_free(operation_json);
    }
    else if(g_strcmp0(name, "finish_edit_plan") == 0)
    {
      JsonParser *finish_parser = json_parser_new();
      if(json_parser_load_from_data(finish_parser, arguments ? arguments : "{}", -1, NULL))
      {
        JsonNode *finish_root = json_parser_get_root(finish_parser);
        if(finish_root && JSON_NODE_HOLDS_OBJECT(finish_root))
        {
          JsonObject *finish = json_node_get_object(finish_root);
          saw_finish = TRUE;

          if(json_object_has_member(finish, "summary"))
          {
            g_free(summary);
            summary = _json_node_dup_string(json_object_get_member(finish, "summary"));
          }

          if(json_object_has_member(finish, "warnings"))
          {
            JsonNode *warnings_node = json_object_get_member(finish, "warnings");
            if(warnings_node && JSON_NODE_HOLDS_ARRAY(warnings_node))
            {
              JsonArray *array = json_node_get_array(warnings_node);
              for(guint j = 0; j < json_array_get_length(array); j++)
              {
                gchar *entry = _json_node_dup_string(json_array_get_element(array, j));
                if(entry) g_ptr_array_add(warnings, entry);
              }
            }
          }

          if(json_object_has_member(finish, "unhandled"))
          {
            JsonNode *unhandled_node = json_object_get_member(finish, "unhandled");
            if(unhandled_node && JSON_NODE_HOLDS_ARRAY(unhandled_node))
            {
              JsonArray *array = json_node_get_array(unhandled_node);
              for(guint j = 0; j < json_array_get_length(array); j++)
              {
                gchar *entry = _json_node_dup_string(json_array_get_element(array, j));
                if(entry) g_ptr_array_add(unhandled, entry);
              }
            }
          }
        }
      }
      g_object_unref(finish_parser);
    }
  }

  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "summary");
  json_builder_add_string_value(builder, summary ? summary : _("applied assistant edits"));

  if(warnings->len > 0)
  {
    json_builder_set_member_name(builder, "warnings");
    json_builder_begin_array(builder);
    for(guint i = 0; i < warnings->len; i++)
      json_builder_add_string_value(builder, g_ptr_array_index(warnings, i));
    json_builder_end_array(builder);
  }

  if(unhandled->len > 0)
  {
    json_builder_set_member_name(builder, "unhandled");
    json_builder_begin_array(builder);
    for(guint i = 0; i < unhandled->len; i++)
      json_builder_add_string_value(builder, g_ptr_array_index(unhandled, i));
    json_builder_end_array(builder);
  }

  json_builder_end_object(builder);

  if(!saw_finish && error_message)
    *error_message = g_strdup(_("assistant did not finish the edit plan"));

  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar *data = json_generator_to_data(generator, NULL);
  json_node_free(root);
  g_object_unref(generator);
  g_object_unref(builder);
  g_ptr_array_free(warnings, TRUE);
  g_ptr_array_free(unhandled, TRUE);
  g_free(summary);
  return data;
}

static gchar *_call_openai_chat_completions(const gchar *api_key,
                                            const gchar *model,
                                            const gchar *catalog,
                                            const gchar *prompt,
                                            gchar **error_message)
{
  if(error_message) *error_message = NULL;
  gchar *request_body = _build_openai_tool_request_body(model, catalog, prompt);
  if(!request_body)
  {
    if(error_message) *error_message = g_strdup(_("unable to build OpenAI tool request"));
    return NULL;
  }

  CURL *curl = curl_easy_init();
  if(!curl)
  {
    if(error_message) *error_message = g_strdup(_("unable to initialize network client"));
    g_free(request_body);
    return NULL;
  }

  dt_curl_init(curl, FALSE);

  GString *response = g_string_new(NULL);
  curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_body));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, DT_PHOTO_ASSISTANT_REQUEST_TIMEOUT);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  gchar *auth = g_strdup_printf("Authorization: Bearer %s", api_key);
  headers = curl_slist_append(headers, auth);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  const CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  g_free(auth);
  g_free(request_body);

  if(res != CURLE_OK || http_code != 200)
  {
    gchar *api_error = _extract_openai_error(response->str);
    if(error_message)
    {
      if(api_error)
        *error_message = api_error;
      else if(res != CURLE_OK)
        *error_message = g_strdup_printf(_("network error: %s"), curl_easy_strerror(res));
      else
        *error_message = g_strdup_printf(_("OpenAI API error (HTTP %ld)"), http_code);
    }
    else
      g_free(api_error);

    g_string_free(response, TRUE);
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, response->str, response->len, NULL))
  {
    if(error_message) *error_message = g_strdup(_("unable to parse OpenAI response"));
    g_object_unref(parser);
    g_string_free(response, TRUE);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    if(error_message) *error_message = g_strdup(_("OpenAI response was not a JSON object"));
    g_object_unref(parser);
    g_string_free(response, TRUE);
    return NULL;
  }

  JsonObject *object = json_node_get_object(root);
  if(!json_object_has_member(object, "choices"))
  {
    if(error_message) *error_message = g_strdup(_("OpenAI response did not contain choices"));
    g_object_unref(parser);
    g_string_free(response, TRUE);
    return NULL;
  }

  JsonArray *choices = json_object_get_array_member(object, "choices");
  if(json_array_get_length(choices) == 0)
  {
    if(error_message) *error_message = g_strdup(_("OpenAI response did not contain any choices"));
    g_object_unref(parser);
    g_string_free(response, TRUE);
    return NULL;
  }

  JsonArray *tool_calls = NULL;
  JsonObject *choice = json_array_get_object_element(choices, 0);
  if(!choice || !json_object_has_member(choice, "message"))
  {
    if(error_message) *error_message = g_strdup(_("OpenAI response choice did not contain message"));
    g_object_unref(parser);
    g_string_free(response, TRUE);
    return NULL;
  }

  JsonObject *message = json_object_get_object_member(choice, "message");
  if(!message || !json_object_has_member(message, "tool_calls"))
  {
    if(error_message) *error_message = g_strdup(_("OpenAI response message did not contain any tool calls"));
    g_object_unref(parser);
    g_string_free(response, TRUE);
    return NULL;
  }

  tool_calls = json_object_get_array_member(message, "tool_calls");

  gchar *tool_error = NULL;
  gchar *result = NULL;
  if(tool_calls && json_array_get_length(tool_calls) > 0)
  {
    result = _build_plan_json_from_tool_calls(tool_calls, &tool_error);
  }
  else
  {
    if(error_message) *error_message = g_strdup(_("OpenAI response did not contain any tool calls"));
  }

  g_object_unref(parser);
  g_string_free(response, TRUE);
  if(error_message && !*error_message && tool_error)
    *error_message = tool_error;
  else
    g_free(tool_error);
  return result;
}

static gboolean _json_node_to_bool(JsonNode *node, gboolean *value)
{
  if(!node || !value) return FALSE;

  if(JSON_NODE_HOLDS_VALUE(node))
  {
    GType type = json_node_get_value_type(node);
    if(type == G_TYPE_BOOLEAN)
    {
      *value = json_node_get_boolean(node);
      return TRUE;
    }
    if(type == G_TYPE_INT64 || type == G_TYPE_DOUBLE)
    {
      *value = json_node_get_double(node) != 0.0;
      return TRUE;
    }
    if(type == G_TYPE_STRING)
    {
      const gchar *text = json_node_get_string(node);
      if(!text) return FALSE;
      if(!g_ascii_strcasecmp(text, "true") || !g_ascii_strcasecmp(text, "yes"))
      {
        *value = TRUE;
        return TRUE;
      }
      if(!g_ascii_strcasecmp(text, "false") || !g_ascii_strcasecmp(text, "no"))
      {
        *value = FALSE;
        return TRUE;
      }
    }
  }

  return FALSE;
}

static gboolean _json_node_to_double(JsonNode *node, gdouble *value)
{
  if(!node || !value || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;

  const GType type = json_node_get_value_type(node);
  if(type == G_TYPE_DOUBLE || type == G_TYPE_INT64)
  {
    *value = json_node_get_double(node);
    return TRUE;
  }
  if(type == G_TYPE_STRING)
  {
    const gchar *text = json_node_get_string(node);
    if(!text) return FALSE;
    gchar *endptr = NULL;
    const double parsed = g_ascii_strtod(text, &endptr);
    if(endptr)
    {
      while(*endptr && g_ascii_isspace((guchar)*endptr))
        endptr++;
      if(*endptr == '\0')
      {
        *value = parsed;
        return TRUE;
      }
    }
  }

  return FALSE;
}

static gboolean _set_field_value(dt_iop_module_t *module,
                                 const dt_introspection_field_t *field,
                                 JsonNode *value_node,
                                 GString *details)
{
  if(!module || !field || !value_node) return FALSE;

  guint8 *ptr = (guint8 *)module->params + field->header.offset;
  gboolean changed = FALSE;
  gchar *before = _field_value_to_string(field, module->params);

  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const float clamped = CLAMP((float)value, field->Float.Min, field->Float.Max);
      changed = fabsf(*(float *)ptr - clamped) > 0.00001f;
      *(float *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_DOUBLE:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const double clamped = CLAMP(value, field->Double.Min, field->Double.Max);
      changed = fabs(*(double *)ptr - clamped) > 0.00001;
      *(double *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_INT8:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const int8_t clamped = CLAMP((int)llround(value), field->Int8.Min, field->Int8.Max);
      changed = *(int8_t *)ptr != clamped;
      *(int8_t *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_UINT8:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const uint8_t clamped = CLAMP((int)llround(value), field->UInt8.Min, field->UInt8.Max);
      changed = *(uint8_t *)ptr != clamped;
      *(uint8_t *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_SHORT:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const short clamped = CLAMP((int)llround(value), field->Short.Min, field->Short.Max);
      changed = *(short *)ptr != clamped;
      *(short *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_USHORT:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const unsigned short clamped = CLAMP((int)llround(value), field->UShort.Min, field->UShort.Max);
      changed = *(unsigned short *)ptr != clamped;
      *(unsigned short *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_INT:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const int clamped = CLAMP((int)llround(value), field->Int.Min, field->Int.Max);
      changed = *(int *)ptr != clamped;
      *(int *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_UINT:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const unsigned int clamped = CLAMP((unsigned int)llround(value), field->UInt.Min, field->UInt.Max);
      changed = *(unsigned int *)ptr != clamped;
      *(unsigned int *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_LONG:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const long clamped = CLAMP((long)llround(value), field->Long.Min, field->Long.Max);
      changed = *(long *)ptr != clamped;
      *(long *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_ULONG:
    {
      gdouble value = 0.0;
      if(!_json_node_to_double(value_node, &value)) break;
      const unsigned long clamped = CLAMP((unsigned long)llround(value), field->ULong.Min, field->ULong.Max);
      changed = *(unsigned long *)ptr != clamped;
      *(unsigned long *)ptr = clamped;
      break;
    }
    case DT_INTROSPECTION_TYPE_BOOL:
    {
      gboolean value = FALSE;
      if(!_json_node_to_bool(value_node, &value)) break;
      changed = *(gboolean *)ptr != value;
      *(gboolean *)ptr = value;
      break;
    }
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      int enum_value = 0;
      gboolean ok = FALSE;
      if(JSON_NODE_HOLDS_VALUE(value_node)
         && json_node_get_value_type(value_node) == G_TYPE_STRING)
      {
        const gchar *text = json_node_get_string(value_node);
        if(text)
          ok = dt_introspection_get_enum_value((dt_introspection_field_t *)field, text, &enum_value);
      }
      else
      {
        gdouble value = 0.0;
        if(_json_node_to_double(value_node, &value))
        {
          enum_value = (int)llround(value);
          ok = TRUE;
        }
      }

      if(!ok) break;
      changed = *(int *)ptr != enum_value;
      *(int *)ptr = enum_value;
      break;
    }
    default:
      break;
  }

  if(changed)
  {
    gchar *after = _field_value_to_string(field, module->params);
    g_string_append_printf(details, "    %s: %s -> %s\n", field->header.name, before, after);
    g_free(after);
    g_free(before);
    return TRUE;
  }

  g_free(before);
  return FALSE;
}

static dt_photo_assistant_apply_result_t *_apply_plan_json(const gchar *plan_json, gchar **error_message)
{
  if(error_message) *error_message = NULL;

  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, plan_json, -1, NULL))
  {
    if(error_message) *error_message = g_strdup(_("assistant response was not valid JSON"));
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    if(error_message) *error_message = g_strdup(_("assistant response was not a JSON object"));
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *object = json_node_get_object(root);
  JsonArray *operations = json_object_has_member(object, "operations")
                        ? json_object_get_array_member(object, "operations")
                        : NULL;

  GString *details = g_string_new(NULL);
  gint applied = 0;
  dt_iop_module_t *focus_module = NULL;

  if(operations)
  {
    const guint len = json_array_get_length(operations);
    for(guint i = 0; i < len; i++)
    {
      JsonObject *operation = json_array_get_object_element(operations, i);
      if(!operation || !json_object_has_member(operation, "module")) continue;

      const gchar *module_op = json_object_get_string_member(operation, "module");
      if(!module_op)
      {
        g_string_append(details, "- unable to find module `(missing or non-string name)`\n");
        continue;
      }
      dt_iop_module_so_t *module_so = dt_iop_get_module_so(module_op);
      dt_iop_module_t *module = module_so ? dt_iop_get_module_preferred_instance(module_so)
                                          : dt_iop_get_module(module_op);
      if(!module) module = dt_iop_get_module(module_op);

      if(!module)
      {
        g_string_append_printf(details, "- unable to find module `%s`\n", module_op);
        continue;
      }

      const gboolean had_history = dt_dev_get_history_item(darktable.develop, module->op) != NULL;
      gboolean requested_enable = TRUE;
      if(json_object_has_member(operation, "enable"))
      {
        JsonNode *enable_node = json_object_get_member(operation, "enable");
        _json_node_to_bool(enable_node, &requested_enable);
      }

      gboolean changed = FALSE;
      if(module->enabled != requested_enable)
      {
        if(!module->hide_enable_button || requested_enable)
        {
          module->enabled = requested_enable;
          if(module->off)
          {
            ++darktable.gui->reset;
            dt_iop_gui_set_enable_button(module);
            --darktable.gui->reset;
          }
          changed = TRUE;
        }
      }

      g_string_append_printf(details, "- %s (%s)\n",
                             module->name() ? module->name() : "",
                             module->op);

      if(json_object_has_member(operation, "fields"))
      {
        JsonArray *fields = json_object_get_array_member(operation, "fields");
        for(guint j = 0; j < json_array_get_length(fields); j++)
        {
          JsonObject *field_object = json_array_get_object_element(fields, j);
          if(!field_object
             || !json_object_has_member(field_object, "name")
             || !json_object_has_member(field_object, "value"))
            continue;

          const gchar *field_name = json_object_get_string_member(field_object, "name");
          if(!field_name)
          {
            g_string_append(details, "    skipped field with missing or non-string name\n");
            continue;
          }
          dt_introspection_field_t *field = module->get_f ? module->get_f(field_name) : NULL;
          if(!_field_is_supported(field))
          {
            g_string_append_printf(details, "    skipped unsupported field `%s`\n", field_name);
            continue;
          }

          JsonNode *value_node = json_object_get_member(field_object, "value");
          if(_set_field_value(module, field, value_node, details))
            changed = TRUE;
        }
      }

      if(changed)
      {
        dt_iop_gui_update(module);
        if(!had_history && module->enabled)
          dt_dev_add_new_history_item(darktable.develop, module, module->enabled);
        else
          dt_dev_add_history_item(darktable.develop, module, module->enabled);

        dt_iop_gui_set_expanded(module, TRUE, FALSE);
        if(!focus_module) focus_module = module;
        applied++;
      }
      else
      {
        g_string_append(details, "    no effective change\n");
      }

      if(json_object_has_member(operation, "focus"))
      {
        gboolean focus = FALSE;
        if(_json_node_to_bool(json_object_get_member(operation, "focus"), &focus) && focus)
          focus_module = module;
      }
    }
  }

  if(focus_module)
    dt_iop_request_focus(focus_module);

  if(json_object_has_member(object, "warnings"))
  {
    JsonArray *warnings = json_object_get_array_member(object, "warnings");
    if(json_array_get_length(warnings) > 0)
    {
      g_string_append(details, "\nwarnings:\n");
      for(guint i = 0; i < json_array_get_length(warnings); i++)
      {
        const gchar *w = json_array_get_string_element(warnings, i);
        g_string_append_printf(details, "- %s\n", w ? w : _("(non-string entry)"));
      }
    }
  }

  if(json_object_has_member(object, "unhandled"))
  {
    JsonArray *unhandled = json_object_get_array_member(object, "unhandled");
    if(json_array_get_length(unhandled) > 0)
    {
      g_string_append(details, "\nnot handled:\n");
      for(guint i = 0; i < json_array_get_length(unhandled); i++)
      {
        const gchar *u = json_array_get_string_element(unhandled, i);
        g_string_append_printf(details, "- %s\n", u ? u : _("(non-string entry)"));
      }
    }
  }

  dt_photo_assistant_apply_result_t *result = g_new0(dt_photo_assistant_apply_result_t, 1);
  result->summary = json_object_has_member(object, "summary")
                  ? g_strdup(json_object_get_string_member(object, "summary"))
                  : g_strdup(_("applied assistant edits"));
  result->details = g_string_free(details, FALSE);
  result->operations_applied = applied;

  g_object_unref(parser);
  return result;
}

static void _free_apply_result(dt_photo_assistant_apply_result_t *result)
{
  if(!result) return;
  g_free(result->summary);
  g_free(result->details);
  g_free(result);
}

static void _free_request(dt_photo_assistant_request_t *request)
{
  if(!request) return;
  g_free(request->prompt);
  g_free(request->api_key);
  g_free(request->model);
  g_free(request->catalog);
  g_free(request);
}

static void _free_result(dt_photo_assistant_result_t *result)
{
  if(!result) return;
  g_free(result->status);
  g_free(result->plan_json);
  g_free(result);
}

static gboolean _request_finished(gpointer user_data)
{
  dt_photo_assistant_result_t *result = (dt_photo_assistant_result_t *)user_data;
  dt_lib_module_t *self = result->self;
  dt_lib_photo_assistant_t *d = self->data;

  if(!d)
  {
    _free_result(result);
    return G_SOURCE_REMOVE;
  }

  if(result->request_id != d->request_id)
  {
    _free_result(result);
    return G_SOURCE_REMOVE;
  }

  _set_busy(d, FALSE);
  _set_status(d, result->status);

  if(!result->success)
  {
    _set_text_buffer(GTK_TEXT_VIEW(d->response_view), result->status);
    dt_control_log("%s", result->status);
    _free_result(result);
    return G_SOURCE_REMOVE;
  }

  gchar *apply_error = NULL;
  dt_photo_assistant_apply_result_t *apply = _apply_plan_json(result->plan_json, &apply_error);
  if(!apply)
  {
    _set_text_buffer(GTK_TEXT_VIEW(d->response_view), apply_error ? apply_error : result->plan_json);
    _set_status(d, apply_error ? apply_error : _("unable to apply assistant edits"));
    if(apply_error) dt_control_log("%s", apply_error);
    g_free(apply_error);
    _free_result(result);
    return G_SOURCE_REMOVE;
  }

  gchar *response_text = g_strdup_printf(
    "%s\n\nApplied %d module update%s.\n\n%s",
    apply->summary ? apply->summary : _("applied assistant edits"),
    apply->operations_applied,
    apply->operations_applied == 1 ? "" : "s",
    apply->details ? apply->details : "");
  _set_text_buffer(GTK_TEXT_VIEW(d->response_view), response_text);
  g_free(response_text);

  gchar *status = g_strdup_printf(
    ngettext("applied %d assistant edit", "applied %d assistant edits", apply->operations_applied),
    MAX(apply->operations_applied, 0));
  _set_status(d, status);
  dt_control_log("%s", apply->summary ? apply->summary : status);
  g_free(status);

  _free_apply_result(apply);
  _free_result(result);
  return G_SOURCE_REMOVE;
}

static gpointer _photo_assistant_thread(gpointer user_data)
{
  dt_photo_assistant_request_t *request = (dt_photo_assistant_request_t *)user_data;
  dt_photo_assistant_result_t *result = g_new0(dt_photo_assistant_result_t, 1);
  result->self = request->self;
  result->request_id = request->request_id;
  result->plan_json = _call_openai_chat_completions(
    request->api_key,
    request->model,
    request->catalog,
    request->prompt,
    &result->status);
  result->success = result->plan_json != NULL;
  if(result->success && !result->status)
    result->status = g_strdup(_("assistant plan ready"));

  g_main_context_invoke(NULL, _request_finished, result);
  _free_request(request);
  return NULL;
}

static void _save_key_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_photo_assistant_t *d = self->data;
  const gchar *api_key = gtk_entry_get_text(GTK_ENTRY(d->api_key_entry));

  if(!api_key || !*api_key)
  {
    _set_status(d, _("enter an OpenAI API key first"));
    return;
  }

  if(_save_api_key(api_key))
    _set_status(d, _("saved API key in secure storage"));
  else
    _set_status(d, _("secure storage is unavailable; the key stays in this session only"));
}

static void _send_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_photo_assistant_t *d = self->data;
  if(d->busy) return;

  gchar *prompt = _dup_text_buffer(GTK_TEXT_VIEW(d->prompt_view));
  g_strstrip(prompt);
  if(!prompt[0])
  {
    _set_status(d, _("describe the edit you want first"));
    g_free(prompt);
    return;
  }

  const gchar *api_key = gtk_entry_get_text(GTK_ENTRY(d->api_key_entry));
  if(!api_key || !*api_key)
  {
    _load_saved_api_key_into_entry(d->api_key_entry);
    api_key = gtk_entry_get_text(GTK_ENTRY(d->api_key_entry));
  }

  if(!api_key || !*api_key)
  {
    _set_status(d, _("enter or save an OpenAI API key first"));
    g_free(prompt);
    return;
  }

  const gchar *model_text = gtk_entry_get_text(GTK_ENTRY(d->model_entry));
  const gchar *model = (model_text && *model_text) ? model_text : DT_PHOTO_ASSISTANT_DEFAULT_MODEL;
  dt_conf_set_string(DT_PHOTO_ASSISTANT_MODEL_KEY, model);

  dt_photo_assistant_request_t *request = g_new0(dt_photo_assistant_request_t, 1);
  request->self = self;
  request->request_id = ++d->request_id;
  request->prompt = prompt;
  request->api_key = g_strdup(api_key);
  request->model = g_strdup(model);
  request->catalog = _build_module_catalog();

  _set_busy(d, TRUE);
  _set_status(d, _("asking OpenAI for an edit plan..."));
  _set_text_buffer(GTK_TEXT_VIEW(d->response_view), _("Working..."));

  GThread *thread = g_thread_new("photo-assistant", _photo_assistant_thread, request);
  g_thread_unref(thread);
}

const char *name(dt_lib_module_t *self)
{
  return _("photo assistant");
}

const char *description(dt_lib_module_t *self)
{
  return _("use natural language to apply darkroom module edits");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM;
}

int position(const dt_lib_module_t *self)
{
  return 875;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_photo_assistant_t *d = g_malloc0(sizeof(dt_lib_photo_assistant_t));
  self->data = d;

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(6));
  gtk_container_set_border_width(GTK_CONTAINER(root), DT_PIXEL_APPLY_DPI(6));

  GtkWidget *intro = gtk_label_new(_("Describe a photo edit in natural language and the assistant will map it to darkroom modules."));
  gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
  gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
  gtk_box_pack_start(GTK_BOX(root), intro, FALSE, FALSE, 0);

  GtkWidget *settings = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(settings), DT_PIXEL_APPLY_DPI(4));
  gtk_grid_set_column_spacing(GTK_GRID(settings), DT_PIXEL_APPLY_DPI(6));
  gtk_box_pack_start(GTK_BOX(root), settings, FALSE, FALSE, 0);

  GtkWidget *key_label = dt_ui_label_new(_("OpenAI API key"));
  gtk_grid_attach(GTK_GRID(settings), key_label, 0, 0, 1, 1);

  d->api_key_entry = dt_ui_entry_new(14);
  gtk_entry_set_visibility(GTK_ENTRY(d->api_key_entry), FALSE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->api_key_entry), _("paste your key"));
  gtk_grid_attach(GTK_GRID(settings), d->api_key_entry, 1, 0, 1, 1);

  d->save_key_button = dt_action_button_new(self, N_("save key"), G_CALLBACK(_save_key_clicked), self,
                                            _("save the current API key using darktable's password storage backend"), 0, 0);
  gtk_widget_set_hexpand(d->save_key_button, FALSE);
  gtk_grid_attach(GTK_GRID(settings), d->save_key_button, 2, 0, 1, 1);

  GtkWidget *model_label = dt_ui_label_new(_("model"));
  gtk_grid_attach(GTK_GRID(settings), model_label, 0, 1, 1, 1);

  d->model_entry = dt_ui_entry_new(14);
  gchar *saved_model = dt_conf_get_string(DT_PHOTO_ASSISTANT_MODEL_KEY);
  gtk_entry_set_text(GTK_ENTRY(d->model_entry),
                     saved_model && *saved_model ? saved_model : DT_PHOTO_ASSISTANT_DEFAULT_MODEL);
  g_free(saved_model);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->model_entry), DT_PHOTO_ASSISTANT_DEFAULT_MODEL);
  gtk_grid_attach(GTK_GRID(settings), d->model_entry, 1, 1, 2, 1);

  _load_saved_api_key_into_entry(d->api_key_entry);

  GtkWidget *prompt_label = dt_ui_label_new(_("request"));
  gtk_box_pack_start(GTK_BOX(root), prompt_label, FALSE, FALSE, 0);

  GtkWidget *prompt_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(prompt_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(prompt_scroll), DT_PIXEL_APPLY_DPI(96));
  d->prompt_view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->prompt_view), GTK_WRAP_WORD_CHAR);
  gtk_container_add(GTK_CONTAINER(prompt_scroll), d->prompt_view);
  gtk_box_pack_start(GTK_BOX(root), prompt_scroll, FALSE, FALSE, 0);

  d->send_button = dt_action_button_new(self, N_("apply request"), G_CALLBACK(_send_clicked), self,
                                        _("ask OpenAI to translate this request into darkroom module edits"), 0, 0);
  gtk_box_pack_start(GTK_BOX(root), d->send_button, FALSE, FALSE, 0);

  d->status_label = dt_ui_label_new("");
  gtk_box_pack_start(GTK_BOX(root), d->status_label, FALSE, FALSE, 0);

  GtkWidget *response_label = dt_ui_label_new(_("assistant log"));
  gtk_box_pack_start(GTK_BOX(root), response_label, FALSE, FALSE, 0);

  GtkWidget *response_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(response_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(response_scroll), DT_PIXEL_APPLY_DPI(180));
  gtk_widget_set_vexpand(response_scroll, TRUE);
  d->response_view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->response_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(d->response_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(d->response_view), FALSE);
  gtk_container_add(GTK_CONTAINER(response_scroll), d->response_view);
  gtk_box_pack_start(GTK_BOX(root), response_scroll, TRUE, TRUE, 0);

  _set_text_buffer(GTK_TEXT_VIEW(d->response_view),
                   _("Examples:\n"
                     "- add a soft vignette and darken the edges slightly\n"
                     "- make the shadows darker and cooler, then increase global saturation a touch\n"
                     "- add a hazy cinematic look\n"
                     "- rotate the hue slightly toward teal and boost vibrance"));

  self->widget = root;
  gtk_widget_show_all(self->widget);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}
