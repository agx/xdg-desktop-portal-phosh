/*
 * Copyright © 2018 Igalia S.L.
 *             2024 The Phosh Developers
 *
 * Heavily based on the xdg-desktop-portal-gnome
 *
 * Authors:
 *       Patrick Griffis <pgriffis@igalia.com>
 *       Guido Günther <agx@sigxcpu.org>
 */

#include "pmp-config.h"

#include <adwaita.h>
#include <time.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gdesktop-enums.h>

#include "pmp-settings.h"
#include "pmp-utils.h"

#include "xdg-desktop-portal-dbus.h"
#include "fc-monitor.h"

static GHashTable *settings_hash;
static FcMonitor *fontconfig_monitor;
static int fontconfig_serial;
static gboolean enable_animations;

static void sync_animations_enabled (PmpImplSettings *impl);

typedef struct {
  GSettingsSchema *schema;
  GSettings       *settings;
} SettingsBundle;

static SettingsBundle *
settings_bundle_new (GSettingsSchema *schema, GSettings *settings)
{
  SettingsBundle *bundle = g_new (SettingsBundle, 1);

  bundle->schema = schema;
  bundle->settings = settings;
  return bundle;
}

static void
settings_bundle_free (SettingsBundle *bundle)
{
  g_object_unref (bundle->schema);
  g_object_unref (bundle->settings);
  g_free (bundle);
}

static gboolean
namespace_matches (const char         *namespace,
                   const char * const *patterns)
{
  size_t i;

  for (i = 0; patterns[i]; ++i) {
    size_t pattern_len;
    const char *pattern = patterns[i];

    if (pattern[0] == '\0')
      return TRUE;
    if (strcmp (namespace, pattern) == 0)
      return TRUE;

    pattern_len = strlen (pattern);
    if (pattern[pattern_len - 1] == '*' && strncmp (namespace, pattern, pattern_len - 1) == 0)
      return TRUE;
  }

  if (i == 0) /* Empty array */
    return TRUE;

  return FALSE;
}

static GVariant *
get_accent_color (void)
{
  SettingsBundle *bundle = g_hash_table_lookup (settings_hash, "org.gnome.desktop.interface");
  AdwAccentColor color;
  GdkRGBA color_rgba;

  switch (g_settings_get_enum (bundle->settings, "accent-color")) {
  case G_DESKTOP_ACCENT_COLOR_TEAL:
    color = ADW_ACCENT_COLOR_TEAL;
    break;
  case G_DESKTOP_ACCENT_COLOR_GREEN:
    color = ADW_ACCENT_COLOR_GREEN;
    break;
  case G_DESKTOP_ACCENT_COLOR_YELLOW:
    color = ADW_ACCENT_COLOR_YELLOW;
    break;
  case G_DESKTOP_ACCENT_COLOR_ORANGE:
    color = ADW_ACCENT_COLOR_ORANGE;
    break;
  case G_DESKTOP_ACCENT_COLOR_RED:
    color = ADW_ACCENT_COLOR_RED;
    break;
  case G_DESKTOP_ACCENT_COLOR_PINK:
    color = ADW_ACCENT_COLOR_PINK;
    break;
  case G_DESKTOP_ACCENT_COLOR_PURPLE:
    color = ADW_ACCENT_COLOR_PURPLE;
    break;
  case G_DESKTOP_ACCENT_COLOR_SLATE:
    color = ADW_ACCENT_COLOR_SLATE;
    break;
  case G_DESKTOP_ACCENT_COLOR_BLUE:
  default:
    color = ADW_ACCENT_COLOR_BLUE;
  }

  adw_accent_color_to_rgba (color, &color_rgba);

  return g_variant_new ("(ddd)", color_rgba.red, color_rgba.green, color_rgba.blue);
}

static GVariant *
get_color_scheme (void)
{
  SettingsBundle *bundle = g_hash_table_lookup (settings_hash, "org.gnome.desktop.interface");
  int color_scheme;

  if (!g_settings_schema_has_key (bundle->schema, "color-scheme"))
    return g_variant_new_uint32 (0); /* No preference */

  color_scheme = g_settings_get_enum (bundle->settings, "color-scheme");

  return g_variant_new_uint32 (color_scheme);
}


static gboolean
get_contrast (void)
{
  SettingsBundle *bundle = g_hash_table_lookup (settings_hash, "org.gnome.desktop.a11y.interface");
  gboolean hc = FALSE;

  if (bundle && g_settings_schema_has_key (bundle->schema, "high-contrast"))
    hc = g_settings_get_boolean (bundle->settings, "high-contrast");

  return hc;
}


/* The high-contrast value as a GVariant 'u' */
static GVariant *
get_contrast_value (void)
{
  gboolean hc = get_contrast ();
  return g_variant_new_uint32 (hc ? 1 : 0);
}


static GVariant *
get_theme_value (const char *key)
{
  SettingsBundle *bundle = g_hash_table_lookup (settings_hash, "org.gnome.desktop.a11y.interface");
  g_autofree char *theme = NULL;
  gboolean hc = FALSE;

  if (bundle && g_settings_schema_has_key (bundle->schema, "high-contrast"))
    hc = g_settings_get_boolean (bundle->settings, "high-contrast");

  if (hc)
    return g_variant_new_string ("HighContrast");

  bundle = g_hash_table_lookup (settings_hash, "org.gnome.desktop.interface");
  theme = g_settings_get_string (bundle->settings, key);

  return g_variant_new_string (theme);
}

static gboolean
settings_handle_read_all (PmpImplSettings       *object,
                          GDBusMethodInvocation *invocation,
                          const char * const    *arg_namespaces,
                          gpointer               data)
{
  g_autoptr (GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("(a{sa{sv}})"));
  GHashTableIter iter;
  char *key;
  SettingsBundle *value;

  g_variant_builder_open (builder, G_VARIANT_TYPE ("a{sa{sv}}"));

  g_hash_table_iter_init (&iter, settings_hash);
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&value))
  {
    g_auto (GStrv) keys = NULL;
    GVariantDict dict;
    gsize i;

    if (!namespace_matches (key, arg_namespaces))
      continue;

    keys = g_settings_schema_list_keys (value->schema);
    g_variant_dict_init (&dict, NULL);
    for (i = 0; keys[i]; ++i) {
      if (strcmp (key, "org.gnome.desktop.interface") == 0 &&
          strcmp (keys[i], "enable-animations") == 0)
        g_variant_dict_insert_value (&dict, keys[i], g_variant_new_boolean (enable_animations));
      else if (strcmp (key, "org.gnome.desktop.interface") == 0 && strcmp (keys[i], "gtk-theme") == 0)
        g_variant_dict_insert_value (&dict, keys[i], get_theme_value (keys[i]));
      else
        g_variant_dict_insert_value (&dict, keys[i], g_settings_get_value (value->settings, keys[i]));
    }

    g_variant_builder_add (builder, "{s@a{sv}}", key, g_variant_dict_end (&dict));
  }

  if (namespace_matches ("org.gnome.fontconfig", arg_namespaces)) {
    GVariantDict dict;

    g_variant_dict_init (&dict, NULL);
    g_variant_dict_insert_value (&dict, "serial", g_variant_new_int32 (fontconfig_serial));

    g_variant_builder_add (builder, "{s@a{sv}}", "org.gnome.fontconfig", g_variant_dict_end (&dict));
  }

  if (namespace_matches ("org.freedesktop.appearance", arg_namespaces)) {
    GVariantDict dict;

    g_variant_dict_init (&dict, NULL);
    g_variant_dict_insert_value (&dict, "accent-color", get_accent_color ());
    g_variant_dict_insert_value (&dict, "color-scheme", get_color_scheme ());
    g_variant_dict_insert_value (&dict, "contrast", get_contrast_value ());

    g_variant_builder_add (builder, "{s@a{sv}}", "org.freedesktop.appearance", g_variant_dict_end (&dict));
  }

  g_variant_builder_close (builder);

  g_dbus_method_invocation_return_value (invocation, g_variant_builder_end (builder));

  return TRUE;
}

static gboolean
settings_handle_read (PmpImplSettings       *object,
                      GDBusMethodInvocation *invocation,
                      const char            *arg_namespace,
                      const char            *arg_key,
                      gpointer               data)
{
  g_debug ("Read %s %s", arg_namespace, arg_key);

  if (strcmp (arg_namespace, "org.gnome.fontconfig") == 0) {
    if (strcmp (arg_key, "serial") == 0) {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(v)", g_variant_new_int32 (fontconfig_serial)));
      return TRUE;
    }
  } else if (strcmp (arg_namespace, "org.freedesktop.appearance") == 0) {
    if (strcmp (arg_key, "color-scheme") == 0) {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(v)", get_color_scheme ()));
      return TRUE;
    } else if (strcmp (arg_key, "contrast") == 0) {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(v)", get_contrast_value ()));
      return TRUE;
    } else if (strcmp (arg_key, "accent-color") == 0) {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(v)", get_accent_color ()));
      return TRUE;
    }
  } else if (strcmp (arg_namespace, "org.gnome.desktop.interface") == 0 &&
            strcmp (arg_key, "enable-animations") == 0) {
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(v)", g_variant_new_boolean (enable_animations)));
    return TRUE;
  } else if (strcmp (arg_namespace, "org.gnome.desktop.interface") == 0 &&
             (strcmp (arg_key, "gtk-theme") == 0)) {
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(v)", get_theme_value (arg_key)));
    return TRUE;
  } else if (g_hash_table_contains (settings_hash, arg_namespace))    {
    SettingsBundle *bundle = g_hash_table_lookup (settings_hash, arg_namespace);
    if (g_settings_schema_has_key (bundle->schema, arg_key)) {
      g_autoptr (GVariant) variant = NULL;

      variant = g_settings_get_value (bundle->settings, arg_key);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", variant));
      return TRUE;
    }
  }

  g_debug ("Attempted to read unknown namespace/key pair: %s %s", arg_namespace, arg_key);
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                                 _("Requested setting not found"));

  return TRUE;
}

typedef struct {
  PmpImplSettings *self;
  const char      *namespace;
} ChangedSignalUserData;

static ChangedSignalUserData *
changed_signal_user_data_new (PmpImplSettings *settings,
                              const char      *namespace)
{
  ChangedSignalUserData *data = g_new (ChangedSignalUserData, 1);

  data->self = settings;
  data->namespace = namespace;
  return data;
}

static void
changed_signal_user_data_destroy (gpointer  data,
                                  GClosure *closure)
{
  g_free (data);
}

static void
on_settings_changed (GSettings             *settings,
                     const char            *key,
                     ChangedSignalUserData *user_data)
{
  g_autoptr (GVariant) new_value = g_settings_get_value (settings, key);

  g_debug ("Emitting changed for %s %s", user_data->namespace, key);
  if (strcmp (user_data->namespace, "org.gnome.desktop.interface") == 0 &&
      strcmp (key, "enable-animations") == 0)
    sync_animations_enabled (user_data->self);
  else
    pmp_impl_settings_emit_setting_changed (user_data->self,
                                            user_data->namespace, key,
                                            g_variant_new ("v", new_value));

  if (strcmp (user_data->namespace, "org.gnome.desktop.interface") == 0 &&
      strcmp (key, "accent-color") == 0)
    pmp_impl_settings_emit_setting_changed (user_data->self,
                                            "org.freedesktop.appearance", key,
                                            g_variant_new ("v", get_accent_color ()));

  if (strcmp (user_data->namespace, "org.gnome.desktop.interface") == 0 &&
      strcmp (key, "color-scheme") == 0)
    pmp_impl_settings_emit_setting_changed (user_data->self,
                                            "org.freedesktop.appearance", key,
                                            g_variant_new ("v", get_color_scheme ()));

  if (strcmp (user_data->namespace, "org.gnome.desktop.a11y.interface") == 0 &&
      strcmp (key, "high-contrast") == 0) {
    pmp_impl_settings_emit_setting_changed (user_data->self,
                                            "org.gnome.desktop.interface", "gtk-theme",
                                            g_variant_new ("v", get_theme_value ("gtk-theme")));
    if (g_variant_is_of_type (new_value, G_VARIANT_TYPE_BOOLEAN)) {
      gboolean hc = g_variant_get_boolean (new_value);
      pmp_impl_settings_emit_setting_changed (user_data->self,
                                              "org.freedesktop.appearance",
                                              "contrast",
                                              g_variant_new ("v", g_variant_new_uint32 (hc ? 1 : 0)));
    }
  }
}


static void
init_settings_table (PmpImplSettings *settings,
                     GHashTable      *table)
{
  static const char * const schemas[] = {
    "org.gnome.desktop.a11y",
    "org.gnome.desktop.a11y.interface",
    "org.gnome.desktop.calendar",
    "org.gnome.desktop.input-sources",
    "org.gnome.desktop.interface",
    "org.gnome.desktop.privacy",
    "org.gnome.desktop.sound",
    "org.gnome.desktop.wm.preferences",
    "org.gnome.settings-daemon.peripherals.mouse",
    "org.gnome.settings-daemon.plugins.xsettings",
  };
  size_t i;
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();

  for (i = 0; i < G_N_ELEMENTS (schemas); ++i) {
    GSettings *setting;
    GSettingsSchema *schema;
    SettingsBundle *bundle;
    const char *schema_name = schemas[i];

    schema = g_settings_schema_source_lookup (source, schema_name, TRUE);
    if (!schema) {
      g_debug ("%s schema not found", schema_name);
      continue;
    }

    setting = g_settings_new (schema_name);
    bundle = settings_bundle_new (schema, setting);
    g_signal_connect_data (setting, "changed", G_CALLBACK (on_settings_changed),
                           changed_signal_user_data_new (settings, schema_name),
                           changed_signal_user_data_destroy, 0);
    g_hash_table_insert (table, (char*)schema_name, bundle);
  }
}

static void
fontconfig_changed (FcMonitor       *monitor,
                    PmpImplSettings *impl)
{
  const char *namespace = "org.gnome.fontconfig";
  const char *key = "serial";

  g_debug ("Emitting changed for %s %s", namespace, key);

  fontconfig_serial++;

  pmp_impl_settings_emit_setting_changed (impl,
                                          namespace, key,
                                          g_variant_new ("v", g_variant_new_int32 (fontconfig_serial)));
}

static void
set_enable_animations (PmpImplSettings *impl,
                       gboolean         new_enable_animations)
{
  const char *namespace = "org.gnome.desktop.interface";
  const char *key = "enable-animations";
  GVariant *enable_animations_variant;

  if (enable_animations == new_enable_animations)
    return;

  enable_animations = new_enable_animations;
  enable_animations_variant =
    g_variant_new ("v", g_variant_new_boolean (enable_animations));
  pmp_impl_settings_emit_setting_changed (impl,
                                          namespace,
                                          key,
                                          enable_animations_variant);
}

static void
sync_animations_enabled (PmpImplSettings *impl)

{
  SettingsBundle *bundle = g_hash_table_lookup (settings_hash, "org.gnome.desktop.interface");
  gboolean new_enable_animations;

  new_enable_animations = g_settings_get_boolean (bundle->settings, "enable-animations");

  set_enable_animations (impl, new_enable_animations);
}


gboolean
pmp_settings_init (GDBusConnection *bus, GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (pmp_impl_settings_skeleton_new ());

  g_signal_connect (helper, "handle-read", G_CALLBACK (settings_handle_read), NULL);
  g_signal_connect (helper, "handle-read-all", G_CALLBACK (settings_handle_read_all), NULL);

  settings_hash = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)settings_bundle_free);

  init_settings_table (PMP_IMPL_SETTINGS (helper), settings_hash);

  fontconfig_monitor = fc_monitor_new ();
  g_signal_connect (fontconfig_monitor, "updated", G_CALLBACK (fontconfig_changed), helper);
  fc_monitor_start (fontconfig_monitor);

  sync_animations_enabled (PMP_IMPL_SETTINGS (helper));

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;

}
