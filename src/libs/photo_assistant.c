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

#include "common/curl_tools.h"
#include "common/darktable.h"
#include "common/introspection.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <curl/curl.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>

#include <stdlib.h>
#include <string.h>

DT_MODULE(1);

#define OPENAI_URL "https://api.openai.com/v1/chat/completions"

typedef struct dt_lib_photo_assistant_t
{
  GtkWidget *chat_view;
  GtkTextBuffer *chat_buffer;
  GtkWidget *input;
  GtkWidget *api_key_entry;
  GtkWidget *model_combo;
  GtkWidget *send_btn;
  volatile gboolean busy;
  guint pending_requests;
  gboolean cleanup_started;
} dt_lib_photo_assistant_t;

typedef struct _request_ctx_t
{
  dt_lib_photo_assistant_t *data;
  gchar *api_key;
  gchar *model;
  gchar *user_prompt;
  gchar *assistant_raw;
  gchar *error_message;
} _request_ctx_t;

static void _request_ctx_free(_request_ctx_t *ctx)
{
  if(!ctx) return;
  g_free(ctx->api_key);
  g_free(ctx->model);
  g_free(ctx->user_prompt);
  g_free(ctx->assistant_raw);
  g_free(ctx->error_message);
  g_free(ctx);
}

static void _append_chat(dt_lib_photo_assistant_t *d, const char *role, const char *text)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(d->chat_buffer, &end);
  const char *prefix = (g_strcmp0(role, "user") == 0) ? "\n--- You ---\n" : "\n--- Assistant ---\n";
  gtk_text_buffer_insert(d->chat_buffer, &end, prefix, -1);
  gtk_text_buffer_insert(d->chat_buffer, &end, text, -1);
  gtk_text_buffer_insert(d->chat_buffer, &end, "\n", -1);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(d->chat_view), &end, 0.0, FALSE, 0.0, 0.0);
}

static size_t _curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  GString *buf = (GString *)userdata;
  const size_t realsize = size * nmemb;
  g_string_append_len(buf, ptr, (gssize)realsize);
  return realsize;
}

static gboolean _strip_json_fence(gchar *s)
{
  gchar *start = g_strstr_len(s, -1, "```");
  if(!start) return FALSE;
  start += 3;
  if(g_str_has_prefix(start, "json"))
    start += 4;
  while(*start == '\n' || *start == '\r')
    start++;
  gchar *end = g_strstr_len(start, -1, "```");
  if(!end) return FALSE;
  *end = '\0';
  memmove(s, start, (gsize)(end - start + 1));
  return TRUE;
}

static gboolean _apply_param_value(dt_iop_module_t *mod,
                                   dt_introspection_field_t *field,
                                   gpointer params_base,
                                   JsonNode *val,
                                   GString *err)
{
  gpointer p = (uint8_t *)params_base + field->header.offset;

  switch(field->header.type)
  {
  case DT_INTROSPECTION_TYPE_FLOAT:
    if(!JSON_NODE_HOLDS_VALUE(val))
    {
      g_string_append_printf(err, "field `%s` expects a number", field->header.field_name);
      return FALSE;
    }
    {
      float v = 0.0f;
      if(json_node_get_value_type(val) == G_TYPE_DOUBLE)
        v = (float)json_node_get_double(val);
      else if(json_node_get_value_type(val) == G_TYPE_INT64)
        v = (float)json_node_get_int(val);
      else
      {
        g_string_append_printf(err, "field `%s` expects a number", field->header.field_name);
        return FALSE;
      }
      if(v < field->Float.Min) v = field->Float.Min;
      if(v > field->Float.Max) v = field->Float.Max;
      *(float *)p = v;
    }
    break;
  case DT_INTROSPECTION_TYPE_INT:
    if(!JSON_NODE_HOLDS_VALUE(val))
    {
      g_string_append_printf(err, "field `%s` expects an integer", field->header.field_name);
      return FALSE;
    }
    {
      int v = 0;
      if(json_node_get_value_type(val) == G_TYPE_INT64)
        v = (int)json_node_get_int(val);
      else if(json_node_get_value_type(val) == G_TYPE_DOUBLE)
        v = (int)json_node_get_double(val);
      else
      {
        g_string_append_printf(err, "field `%s` expects an integer", field->header.field_name);
        return FALSE;
      }
      if(v < field->Int.Min) v = field->Int.Min;
      if(v > field->Int.Max) v = field->Int.Max;
      *(int *)p = v;
    }
    break;
  case DT_INTROSPECTION_TYPE_BOOL:
    if(!JSON_NODE_HOLDS_VALUE(val) || json_node_get_value_type(val) != G_TYPE_BOOLEAN)
    {
      g_string_append_printf(err, "field `%s` expects a boolean", field->header.field_name);
      return FALSE;
    }
    *(gboolean *)p = json_node_get_boolean(val);
    break;
  case DT_INTROSPECTION_TYPE_ENUM:
    if(!JSON_NODE_HOLDS_VALUE(val))
    {
      g_string_append_printf(err, "field `%s` expects an enum (int)", field->header.field_name);
      return FALSE;
    }
    {
      int v = 0;
      if(json_node_get_value_type(val) == G_TYPE_INT64)
        v = (int)json_node_get_int(val);
      else if(json_node_get_value_type(val) == G_TYPE_DOUBLE)
        v = (int)json_node_get_double(val);
      else
      {
        g_string_append_printf(err, "field `%s` expects an enum (int)", field->header.field_name);
        return FALSE;
      }
      gboolean ok = FALSE;
      for(dt_introspection_type_enum_tuple_t *i = field->Enum.values; i && i->name; i++)
      {
        if(i->value == v)
        {
          ok = TRUE;
          break;
        }
      }
      if(!ok)
      {
        g_string_append_printf(err, "invalid enum value for `%s`", field->header.field_name);
        return FALSE;
      }
      *(int *)p = v;
    }
    break;
  default:
    g_string_append_printf(err, "unsupported type for `%s`", field->header.field_name);
    return FALSE;
  }
  return TRUE;
}

static gboolean _apply_actions_json(const char *json_text, GString *log)
{
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  if(!json_parser_load_from_data(parser, json_text, -1, &error))
  {
    g_string_append_printf(log, _("Could not parse assistant JSON: %s\n"), error->message);
    g_clear_error(&error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_string_append(log, _("Assistant reply is not a JSON object.\n"));
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);
  if(!json_object_has_member(obj, "actions"))
  {
    g_string_append(log, _("No \"actions\" array in assistant reply.\n"));
    g_object_unref(parser);
    return FALSE;
  }

  JsonArray *actions = json_object_get_array_member(obj, "actions");
  const guint n = json_array_get_length(actions);
  gboolean any = FALSE;

  for(guint i = 0; i < n; i++)
  {
    JsonNode *an = json_array_get_element(actions, i);
    if(!JSON_NODE_HOLDS_OBJECT(an))
      continue;
    JsonObject *a = json_node_get_object(an);
    if(!json_object_has_member(a, "type"))
      continue;
    const gchar *type = json_object_get_string_member(a, "type");

    if(g_strcmp0(type, "enable_module") == 0)
    {
      const gchar *op = json_object_get_string_member(a, "op");
      if(!op)
      {
        g_string_append(log, _("enable_module: missing op\n"));
        continue;
      }
      dt_iop_module_t *mod = dt_iop_get_module(op);
      if(!mod)
      {
        g_string_append_printf(log, _("Unknown module `%s`\n"), op);
        continue;
      }
      gboolean en = TRUE;
      if(json_object_has_member(a, "enable"))
        en = json_object_get_boolean_member(a, "enable");
      mod->enabled = en;
      dt_dev_add_history_item(darktable.develop, mod, en);
      g_string_append_printf(log, _("%s: %s\n"), op, en ? _("enabled") : _("disabled"));
      any = TRUE;
    }
    else if(g_strcmp0(type, "reset_module") == 0)
    {
      const gchar *op = json_object_get_string_member(a, "op");
      if(!op)
      {
        g_string_append(log, _("reset_module: missing op\n"));
        continue;
      }
      dt_iop_module_t *mod = dt_iop_get_module(op);
      if(!mod)
      {
        g_string_append_printf(log, _("Unknown module `%s`\n"), op);
        continue;
      }
      dt_iop_load_default_params(mod);
      memcpy(mod->params, mod->default_params, mod->params_size);
      dt_dev_add_history_item(darktable.develop, mod, mod->enabled);
      g_string_append_printf(log, _("Reset parameters for `%s`\n"), op);
      any = TRUE;
    }
    else if(g_strcmp0(type, "set_param") == 0)
    {
      const gchar *op = json_object_get_string_member(a, "op");
      const gchar *field = json_object_get_string_member(a, "field");
      if(!op || !field || !json_object_has_member(a, "value"))
      {
        g_string_append(log, _("set_param: need op, field, value\n"));
        continue;
      }
      dt_iop_module_t *mod = dt_iop_get_module(op);
      if(!mod)
      {
        g_string_append_printf(log, _("Unknown module `%s`\n"), op);
        continue;
      }
      if(!mod->so->have_introspection)
      {
        g_string_append_printf(log, _("Module `%s` has no introspection\n"), op);
        continue;
      }
      dt_introspection_field_t *f = mod->get_f(field);
      if(!f)
      {
        g_string_append_printf(log, _("Unknown field `%s` on `%s`\n"), field, op);
        continue;
      }
      JsonNode *val = json_object_get_member(a, "value");
      GString *perr = g_string_new(NULL);
      if(!_apply_param_value(mod, f, mod->params, val, perr))
      {
        g_string_append_printf(log, "%s\n", perr->str);
        g_string_free(perr, TRUE);
        continue;
      }
      g_string_free(perr, TRUE);
      dt_dev_add_history_item(darktable.develop, mod, mod->enabled);
      g_string_append_printf(log, _("Set `%s`.`%s`\n"), op, field);
      any = TRUE;
    }
    else if(g_strcmp0(type, "note") == 0)
    {
      if(json_object_has_member(a, "text"))
        g_string_append_printf(log, "%s\n", json_object_get_string_member(a, "text"));
    }
  }

  g_object_unref(parser);

  if(any)
    dt_dev_reprocess_center(darktable.develop);

  return any;
}

static char *_openai_chat_sync(const char *api_key,
                               const char *model,
                               const char *user_message,
                               GString *err_out)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "model");
  json_builder_add_string_value(b, model);
  json_builder_set_member_name(b, "temperature");
  json_builder_add_double_value(b, 0.2);
  json_builder_set_member_name(b, "response_format");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "json_object");
  json_builder_end_object(b);

  json_builder_set_member_name(b, "messages");
  json_builder_begin_array(b);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "role");
  json_builder_add_string_value(b, "system");
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(
      b,
      "You control darktable darkroom modules. Always respond with a single JSON object only, no markdown.\n"
      "Schema:\n"
      "{\"reply\":\"short user-facing text\",\"actions\":[{\"type\":\"enable_module\",\"op\":\"string\","
      "\"enable\":true},{\"type\":\"reset_module\",\"op\":\"string\"},{\"type\":\"set_param\","
      "\"op\":\"string\",\"field\":\"string\",\"value\":number|boolean},{\"type\":\"note\",\"text\":\"string\"}]}\n"
      "Internal module op names (use exactly): vignette, colorbalancergb, diffuse, blurs.\n"
      "- vignette: fall-off (scale), fall-off radius (falloff_scale), brightness (-1..1, negative darkens edges), "
      "saturation (-1..1), center.x and center.y (-1..1).\n"
      "- colorbalancergb: hue_angle (-180..180), saturation_global (-1..1), global_H (0..360), global_C (0..1), "
      "vibrance (-1..1). Use for hue/saturation shifts.\n"
      "- diffuse: iterations (int), radius (int), sharpness (-1..1), first..fourth (-1..1) diffusion speeds, "
      "anisotropy_first..fourth (-10..10). Use mild positive 'first' and iterations 2-6 for haze/glow; "
      "negative sharpness with iterations for blur-like diffusion.\n"
      "- blurs: radius (int 4-128), type 0=lens 1=motion. Use for explicit blur.\n"
      "Enable a module before or when setting params. For \"remove\" regions, explain that drawn/masked edits "
      "need the user to use masks in the UI; optionally suggest enabling colorbalancergb with drawn mask.\n"
      "Prefer small parameter changes. If the request is unclear, use a short reply and empty actions or a note.");
  json_builder_end_object(b);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "role");
  json_builder_add_string_value(b, "user");
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, user_message);
  json_builder_end_object(b);
  json_builder_end_array(b);
  json_builder_end_object(b);

  JsonNode *req_root = json_builder_get_root(b);
  g_object_unref(b);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, req_root);
  gchar *body = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  json_node_free(req_root);

  GString *resp = g_string_new(NULL);
  CURL *curl = curl_easy_init();
  if(!curl)
  {
    g_string_append(err_out, _("Could not init HTTP client\n"));
    g_free(body);
    g_string_free(resp, TRUE);
    return NULL;
  }

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  gchar *auth = g_strdup_printf("Authorization: Bearer %s", api_key);
  headers = curl_slist_append(headers, auth);
  g_free(auth);

  dt_curl_init(curl, FALSE);
  curl_easy_setopt(curl, CURLOPT_URL, OPENAI_URL);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  g_free(body);

  if(res != CURLE_OK)
  {
    g_string_append_printf(err_out, _("HTTP error: %s\n"), curl_easy_strerror(res));
    g_string_free(resp, TRUE);
    return NULL;
  }

  if(http_code < 200 || http_code >= 300)
  {
    g_string_append_printf(err_out, _("API HTTP %ld: %s\n"), http_code, resp->str);
    g_string_free(resp, TRUE);
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  if(!json_parser_load_from_data(parser, resp->str, (gssize)resp->len, &error))
  {
    g_string_append_printf(err_out, _("Invalid API JSON: %s\n"), error->message);
    g_clear_error(&error);
    g_object_unref(parser);
    g_string_free(resp, TRUE);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  char *content = NULL;
  if(root && JSON_NODE_HOLDS_OBJECT(root))
  {
    JsonObject *o = json_node_get_object(root);
    if(json_object_has_member(o, "error"))
    {
      JsonObject *eo = json_object_get_object_member(o, "error");
      const gchar *em = json_object_get_string_member(eo, "message");
      g_string_append_printf(err_out, _("API error: %s\n"), em ? em : "?");
    }
    else if(json_object_has_member(o, "choices"))
    {
      JsonArray *ch = json_object_get_array_member(o, "choices");
      if(json_array_get_length(ch) > 0)
      {
        JsonObject *c0 = json_node_get_object(json_array_get_element(ch, 0));
        if(json_object_has_member(c0, "message"))
        {
          JsonObject *msg = json_object_get_object_member(c0, "message");
          if(json_object_has_member(msg, "content"))
            content = g_strdup(json_object_get_string_member(msg, "content"));
        }
      }
    }
  }
  g_object_unref(parser);
  g_string_free(resp, TRUE);

  if(!content)
  {
    if(err_out->len == 0)
      g_string_append(err_out, _("Empty model response\n"));
    return NULL;
  }

  return content;
}

static gboolean _idle_finish_request(gpointer user_data)
{
  _request_ctx_t *ctx = (_request_ctx_t *)user_data;
  dt_lib_photo_assistant_t *d = ctx->data;
  if(!d)
  {
    _request_ctx_free(ctx);
    return G_SOURCE_REMOVE;
  }

  if(!d->cleanup_started)
  {
    _append_chat(d, "user", ctx->user_prompt);

    GString *combined = g_string_new(NULL);

    if(ctx->error_message)
    {
      g_string_append(combined, ctx->error_message);
    }
    else if(ctx->assistant_raw)
    {
      gchar *parse_buf = g_strdup(ctx->assistant_raw);
      gboolean stripped = _strip_json_fence(parse_buf);
      const char *json_text = stripped ? parse_buf : ctx->assistant_raw;

      JsonParser *jp = json_parser_new();
      GError *je = NULL;
      const gboolean parsed = json_parser_load_from_data(jp, json_text, -1, &je);
      if(parsed)
      {
        JsonNode *root = json_parser_get_root(jp);
        if(root && JSON_NODE_HOLDS_OBJECT(root))
        {
          JsonObject *jo = json_node_get_object(root);
          if(json_object_has_member(jo, "reply"))
            g_string_append_printf(combined, "%s\n", json_object_get_string_member(jo, "reply"));
          GString *act_log = g_string_new(NULL);
          _apply_actions_json(json_text, act_log);
          if(act_log->len)
            g_string_append_printf(combined, "\n%s", act_log->str);
          g_string_free(act_log, TRUE);
        }
        else
          g_string_append(combined, ctx->assistant_raw);
      }
      else
      {
        g_string_append_printf(combined, "%s\n", ctx->assistant_raw);
        if(je)
          g_string_append_printf(combined, _("(Could not parse structured reply: %s)\n"), je->message);
        g_clear_error(&je);
      }
      g_object_unref(jp);
      g_free(parse_buf);
    }

    _append_chat(d, "assistant", combined->str);
    g_string_free(combined, TRUE);

    d->busy = FALSE;
    gtk_widget_set_sensitive(d->send_btn, TRUE);
  }

  if(d->pending_requests > 0) d->pending_requests--;
  if(d->cleanup_started && d->pending_requests == 0)
    g_free(d);

  _request_ctx_free(ctx);
  return G_SOURCE_REMOVE;
}

static gpointer _request_thread(gpointer data)
{
  _request_ctx_t *ctx = (_request_ctx_t *)data;
  GString *err = g_string_new(NULL);
  char *raw = _openai_chat_sync(ctx->api_key, ctx->model, ctx->user_prompt, err);

  g_free(ctx->api_key);
  g_free(ctx->model);
  ctx->api_key = NULL;
  ctx->model = NULL;

  if(raw)
    ctx->assistant_raw = raw;
  else
    ctx->error_message = g_strdup(err->str);

  g_string_free(err, TRUE);

  gdk_threads_add_idle(_idle_finish_request, ctx);
  return NULL;
}

static void _on_send(GtkWidget *w, gpointer user_data)
{
  (void)w;
  dt_lib_module_t *mod = (dt_lib_module_t *)user_data;
  dt_lib_photo_assistant_t *d = mod->data;
  if(!d || d->busy) return;

  const gchar *text = gtk_entry_get_text(GTK_ENTRY(d->input));
  if(!text || !*text) return;

  const gchar *env_key = g_getenv("OPENAI_API_KEY");
  const gchar *entry_key = gtk_entry_get_text(GTK_ENTRY(d->api_key_entry));
  const gchar *api_key = (env_key && *env_key) ? env_key : entry_key;
  if(!api_key || !*api_key)
  {
    dt_control_log(_("Set OPENAI_API_KEY or enter an API key in the photo assistant panel"));
    return;
  }

  gchar *model = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(d->model_combo));
  if(!model || !*model)
  {
    g_free(model);
    model = g_strdup("gpt-4o-mini");
  }

  _request_ctx_t *ctx = g_malloc0(sizeof(_request_ctx_t));
  ctx->data = d;
  ctx->user_prompt = g_strdup(text);
  ctx->api_key = g_strdup(api_key);
  ctx->model = model;

  gtk_entry_set_text(GTK_ENTRY(d->input), "");
  d->busy = TRUE;
  gtk_widget_set_sensitive(d->send_btn, FALSE);
  d->pending_requests++;

  GThread *thread = g_thread_new("photo_assistant_openai", _request_thread, ctx);
  if(thread)
    g_thread_unref(thread);
}

const char *name(dt_lib_module_t *self)
{
  return _("photo assistant");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 950;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_photo_assistant_t *d = g_malloc0(sizeof(dt_lib_photo_assistant_t));
  self->data = d;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(4));
  gtk_widget_set_tooltip_text(box,
                              _("Chat with an OpenAI model to adjust the current image using darkroom modules. "
                                "Set OPENAI_API_KEY or paste a key below."));

  d->chat_view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->chat_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(d->chat_view), FALSE);
  d->chat_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->chat_view));
  gtk_widget_set_size_request(d->chat_view, -1, DT_PIXEL_APPLY_DPI(180));

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(sw), d->chat_view);

  d->input = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->input), _("Describe the edit you want…"));

  d->api_key_entry = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(d->api_key_entry), FALSE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->api_key_entry), _("OpenAI API key (or use OPENAI_API_KEY)"));

  d->model_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->model_combo), "gpt-4o-mini");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->model_combo), "gpt-4o");
  gtk_combo_box_set_active(GTK_COMBO_BOX(d->model_combo), 0);

  d->send_btn = gtk_button_new_with_label(_("Send"));
  gtk_widget_set_hexpand(d->send_btn, TRUE);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  gtk_box_pack_start(GTK_BOX(row), d->input, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(row), d->send_btn, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Model")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), d->model_combo, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("API key")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), d->api_key_entry, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

  self->widget = box;
  g_signal_connect(G_OBJECT(d->send_btn), "clicked", G_CALLBACK(_on_send), self);
  g_signal_connect(G_OBJECT(d->input), "activate", G_CALLBACK(_on_send), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_photo_assistant_t *d = self->data;
  self->data = NULL;
  if(!d) return;
  d->cleanup_started = TRUE;
  if(d->pending_requests == 0)
    g_free(d);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
