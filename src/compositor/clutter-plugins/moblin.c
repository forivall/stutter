/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (c) 2008 Intel Corp.
 *
 * Author: Tomas Frydrych <tf@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#define META_COMPOSITOR_CLUTTER_BUILDING_PLUGIN 1
#include "compositor-clutter-plugin.h"

#include <libintl.h>
#define _(x) dgettext (GETTEXT_PACKAGE, x)
#define N_(x) x

#include <clutter/clutter.h>
#include <gmodule.h>
#include <string.h>

#define DESTROY_TIMEOUT   250
#define MINIMIZE_TIMEOUT  250
#define MAXIMIZE_TIMEOUT  250
#define MAP_TIMEOUT       250
#define SWITCH_TIMEOUT    500

#define ACTOR_DATA_KEY "MCCP-Simple-actor-data"

typedef struct PluginPrivate PluginPrivate;
typedef struct ActorPrivate  ActorPrivate;

static void minimize   (ClutterActor *actor, MetaCompWindowType type,
                        gint workspace);
static void map        (ClutterActor *actor, MetaCompWindowType type,
                        gint workspace);
static void destroy    (ClutterActor *actor, MetaCompWindowType type,
                        gint workspace);
static void maximize   (ClutterActor *actor, MetaCompWindowType type,
                        gint workspace,
                        gint x, gint y, gint width, gint height);
static void unmaximize (ClutterActor *actor, MetaCompWindowType type,
                        gint workspace,
                        gint x, gint y, gint width, gint height);

static void switch_workspace (const GList **actors, gint from, gint to);

static void kill_effect (ClutterActor *actor, gulong event);

static gboolean reload (void);

/*
 * First we create the header struct and initialize its static members.
 * Any dynamically allocated data should be initialized in the
 * init () function below.
 */
MetaCompositorClutterPlugin META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT =
  {
    /*
     * These are predefined values; do not modify.
     */
    .version_major = METACITY_MAJOR_VERSION,
    .version_minor = METACITY_MINOR_VERSION,
    .version_micro = METACITY_MICRO_VERSION,
    .version_api   = METACITY_CLUTTER_PLUGIN_API_VERSION,

    /* Human readable name (for use in UI) */
    .name = "Simple Effects",

    /* Which types of events this plugin supports */
    .features = META_COMPOSITOR_CLUTTER_PLUGIN_MINIMIZE   |
                META_COMPOSITOR_CLUTTER_PLUGIN_DESTROY    |
                META_COMPOSITOR_CLUTTER_PLUGIN_MAP        |
                META_COMPOSITOR_CLUTTER_PLUGIN_MAXIMIZE   |
                META_COMPOSITOR_CLUTTER_PLUGIN_UNMAXIMIZE |
                META_COMPOSITOR_CLUTTER_PLUGIN_SWITCH_WORKSPACE,


    /* And the corresponding handlers */
    .minimize         = minimize,
    .destroy          = destroy,
    .map              = map,
    .maximize         = maximize,
    .unmaximize       = unmaximize,
    .switch_workspace = switch_workspace,

    .kill_effect      = kill_effect,

    /* The reload handler */
    .reload           = reload
  };

/*
 * Plugin private data that we store in the .plugin_private member.
 */
struct PluginPrivate
{
  ClutterEffectTemplate *destroy_effect;
  ClutterEffectTemplate *minimize_effect;
  ClutterEffectTemplate *maximize_effect;
  ClutterEffectTemplate *map_effect;
  ClutterEffectTemplate *switch_workspace_effect;

  /* Valid only when switch_workspace effect is in progress */
  ClutterTimeline       *tml_switch_workspace1;
  ClutterTimeline       *tml_switch_workspace2;
  GList                **actors;
  ClutterActor          *desktop1;
  ClutterActor          *desktop2;

  gboolean               debug_mode : 1;
};

/*
 * Per actor private data we attach to each actor.
 */
struct ActorPrivate
{
  ClutterActor *orig_parent;

  gint          workspace;

  ClutterTimeline *tml_minimize;
  ClutterTimeline *tml_maximize;
  ClutterTimeline *tml_destroy;
  ClutterTimeline *tml_map;

  gboolean      is_minimized : 1;
  gboolean      is_maximized : 1;
};

/*
 * Actor private data accessor
 */
static ActorPrivate *
get_actor_private (ClutterActor *actor)
{
  ActorPrivate * priv = g_object_get_data (G_OBJECT (actor), ACTOR_DATA_KEY);

  if (!priv)
    {
      priv = g_new0 (ActorPrivate, 1);
      g_object_set_data_full (G_OBJECT (actor), ACTOR_DATA_KEY, priv, g_free);
    }

  return priv;
}

static void
on_switch_workspace_effect_complete (ClutterActor *group, gpointer data)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  PluginPrivate               *ppriv  = plugin->plugin_private;
  GList                       *l = *((GList**)data);
  ClutterActor                *actor_for_cb = l->data;

  while (l)
    {
      ClutterActor *a = l->data;
      ActorPrivate *priv = get_actor_private (a);

      if (priv->orig_parent)
        {
          clutter_actor_reparent (a, priv->orig_parent);
          priv->orig_parent = NULL;
        }

      l = l->next;
    }

  clutter_actor_destroy (ppriv->desktop1);
  clutter_actor_destroy (ppriv->desktop2);

  ppriv->actors = NULL;
  ppriv->tml_switch_workspace1 = NULL;
  ppriv->tml_switch_workspace2 = NULL;
  ppriv->desktop1 = NULL;
  ppriv->desktop2 = NULL;

  if (plugin->completed)
    plugin->completed (plugin, actor_for_cb,
                       META_COMPOSITOR_CLUTTER_PLUGIN_SWITCH_WORKSPACE);
}

static void
switch_workspace (const GList **actors, gint from, gint to)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  PluginPrivate               *ppriv  = plugin->plugin_private;
  GList                       *l;
  gint                         n_workspaces;
  ClutterActor                *group1  = clutter_group_new ();
  ClutterActor                *group2  = clutter_group_new ();
  ClutterActor                *stage   = plugin->stage;

#if 1
  clutter_actor_set_anchor_point (group2,
                                  plugin->screen_width,
                                  plugin->screen_height);
  clutter_actor_set_position (group2,
                              plugin->screen_width,
                              plugin->screen_height);
#endif

  clutter_actor_set_scale (group2, 0.0, 0.0);

  clutter_container_add_actor (CLUTTER_CONTAINER (stage), group2);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), group1);

  if (from == to)
    {
      if (plugin->completed)
        plugin->completed (plugin, NULL,
                           META_COMPOSITOR_CLUTTER_PLUGIN_SWITCH_WORKSPACE);
      return;
    }

  n_workspaces = g_list_length (plugin->work_areas);

  l = g_list_last (*((GList**) actors));

  while (l)
    {
      ClutterActor *a = l->data;
      ActorPrivate *priv = get_actor_private (a);
      gint          workspace;

      workspace = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (a),
                              META_COMPOSITOR_CLUTTER_PLUGIN_WORKSPACE_KEY));

      priv->workspace = workspace;

      if (workspace == to || workspace == from)
        {
          gint x, y;
          guint w, h;

          clutter_actor_get_position (a, &x, &y);
          clutter_actor_get_size (a, &w, &h);

          priv->orig_parent = clutter_actor_get_parent (a);

          clutter_actor_reparent (a, workspace == to ? group2 : group1);
          clutter_actor_show_all (a);
          clutter_actor_raise_top (a);
        }
      else if (workspace < 0)
        {
          /* Sticky window */
          priv->orig_parent = NULL;
        }
      else
        {
          /* Window on some other desktop */
          clutter_actor_hide (a);
          priv->orig_parent = NULL;
        }

      l = l->prev;
    }

  ppriv->actors  = (GList **)actors;
  ppriv->desktop1 = group1;
  ppriv->desktop2 = group2;

  ppriv->tml_switch_workspace2 = clutter_effect_scale (
                                           ppriv->switch_workspace_effect,
                                           group2, 1.0, 1.0,
                                           on_switch_workspace_effect_complete,
                                           (gpointer)actors);

  ppriv->tml_switch_workspace1 = clutter_effect_scale (
                                           ppriv->switch_workspace_effect,
                                           group1, 0.0, 0.0,
                                           NULL, NULL);
}


/*
 * Minimize effect completion callback; this function restores actor state, and
 * calls the manager callback function.
 */
static void
on_minimize_effect_complete (ClutterActor *actor, gpointer data)
{
  /*
   * Must reverse the effect of the effect; must hide it first to ensure
   * that the restoration will not be visible.
   */
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  ActorPrivate *apriv                 = get_actor_private (actor);

  apriv->tml_minimize = NULL;

  clutter_actor_hide (actor);

  clutter_actor_set_scale (actor, 1.0, 1.0);
  clutter_actor_move_anchor_point_from_gravity (actor,
                                                CLUTTER_GRAVITY_NORTH_WEST);

  /* Decrease the running effect counter */
  plugin->running--;

  /* Now notify the manager that we are done with this effect */
  if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_MINIMIZE);
}

/*
 * Simple minimize handler: it applies a scale effect (which must be reversed on
 * completion).
 */
static void
minimize (ClutterActor *actor, MetaCompWindowType type, gint workspace)

{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  PluginPrivate               *priv   = plugin->plugin_private;

  if (type == META_COMP_WINDOW_NORMAL)
    {
      ActorPrivate *apriv  = get_actor_private (actor);

      apriv->is_minimized = TRUE;

      clutter_actor_move_anchor_point_from_gravity (actor,
                                                    CLUTTER_GRAVITY_CENTER);

      META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT.running++;

      apriv->tml_minimize = clutter_effect_scale (priv->minimize_effect,
                                                  actor,
                                                  0.0,
                                                  0.0,
                                                  (ClutterEffectCompleteFunc)
                                                  on_minimize_effect_complete,
                                                  NULL);
    }
  else if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_MINIMIZE);
}

/*
 * Minimize effect completion callback; this function restores actor state, and
 * calls the manager callback function.
 */
static void
on_maximize_effect_complete (ClutterActor *actor, gpointer data)
{
  /*
   * Must reverse the effect of the effect.
   */
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  ActorPrivate                *apriv  = get_actor_private (actor);

  apriv->tml_maximize = NULL;

  clutter_actor_set_scale (actor, 1.0, 1.0);
  clutter_actor_move_anchor_point_from_gravity (actor,
                                                CLUTTER_GRAVITY_NORTH_WEST);

  /* Decrease the running effect counter */
  plugin->running--;

  /* Now notify the manager that we are done with this effect */
  if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_MAXIMIZE);
}

/*
 * The Nature of Maximize operation is such that it is difficult to do a visual
 * effect that would work well. Scaling, the obvious effect, does not work that
 * well, because at the end of the effect we end up with window content bigger
 * and differently laid out than in the real window; this is a proof concept.
 *
 * (Something like a sound would be more appropriate.)
 */
static void
maximize (ClutterActor *actor, MetaCompWindowType type, gint workspace,
          gint end_x, gint end_y, gint end_width, gint end_height)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  PluginPrivate               *priv   = plugin->plugin_private;

  gdouble  scale_x    = 1.0;
  gdouble  scale_y    = 1.0;
  gint     anchor_x   = 0;
  gint     anchor_y   = 0;

  if (type == META_COMP_WINDOW_NORMAL)
    {
      ActorPrivate *apriv  = get_actor_private (actor);
      guint width, height;
      gint  x, y;

      apriv->is_maximized = TRUE;

      clutter_actor_get_size (actor, &width, &height);
      clutter_actor_get_position (actor, &x, &y);

      /*
       * Work out the scale and anchor point so that the window is expanding
       * smoothly into the target size.
       */
      scale_x = (gdouble)end_width / (gdouble) width;
      scale_y = (gdouble)end_height / (gdouble) height;

      anchor_x = (gdouble)(x - end_x)*(gdouble)width /
        ((gdouble)(end_width - width));
      anchor_y = (gdouble)(y - end_y)*(gdouble)height /
        ((gdouble)(end_height - height));

      clutter_actor_move_anchor_point (actor, anchor_x, anchor_y);

      apriv->tml_maximize = clutter_effect_scale (priv->maximize_effect,
                                                  actor,
                                                  scale_x,
                                                  scale_y,
                                                  (ClutterEffectCompleteFunc)
                                                  on_maximize_effect_complete,
                                                  NULL);

      return;
    }

  if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_MAXIMIZE);
}

/*
 * See comments on the maximize() function.
 *
 * (Just a skeleton code.)
 */
static void
unmaximize (ClutterActor *actor, MetaCompWindowType type, gint workspace,
            gint end_x, gint end_y, gint end_width, gint end_height)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;

  if (type == META_COMP_WINDOW_NORMAL)
    {
      ActorPrivate *apriv  = get_actor_private (actor);

      apriv->is_maximized = FALSE;

      printf ("Doing unmaximize to target %d,%d;%dx%d\n",
              end_x, end_y, end_width, end_height);
    }

  /* Do this conditionally, if the effect requires completion callback. */
  plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_UNMAXIMIZE);
}

static void
on_map_effect_complete (ClutterActor *actor, gpointer data)
{
  /*
   * Must reverse the effect of the effect.
   */
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  ActorPrivate                *apriv  = get_actor_private (actor);

  apriv->tml_map = NULL;

  clutter_actor_move_anchor_point_from_gravity (actor,
                                                CLUTTER_GRAVITY_NORTH_WEST);

  /* Decrease the running effect counter */
  plugin->running--;

  /* Now notify the manager that we are done with this effect */
  if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_MAP);
}

/*
 * Simple map handler: it applies a scale effect which must be reversed on
 * completion).
 */
static void
map (ClutterActor *actor, MetaCompWindowType type, gint workspace)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  PluginPrivate               *priv   = plugin->plugin_private;

  if (type == META_COMP_WINDOW_NORMAL)
    {
      ActorPrivate *apriv  = get_actor_private (actor);

      clutter_actor_move_anchor_point_from_gravity (actor,
                                                    CLUTTER_GRAVITY_CENTER);

      META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT.running++;

      clutter_actor_set_scale (actor, 0.0, 0.0);
      clutter_actor_show (actor);

      apriv->tml_map = clutter_effect_scale (priv->map_effect,
                                             actor,
                                             1.0,
                                             1.0,
                                             (ClutterEffectCompleteFunc)
                                             on_map_effect_complete,
                                             NULL);

      apriv->is_minimized = FALSE;

    }
  else if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_MAP);
}

/*
 * Destroy effect completion callback; this is a simple effect that requires no
 * further action than decreasing the running effect counter and notifying the
 * manager that the effect is completed.
 */
static void
on_destroy_effect_complete (ClutterActor *actor, gpointer data)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  ActorPrivate                *apriv  = get_actor_private (actor);

  apriv->tml_destroy = NULL;

  plugin->running--;

  if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_DESTROY);
}

/*
 * Simple TV-out like effect.
 */
static void
destroy (ClutterActor *actor, MetaCompWindowType type, gint workspace)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  PluginPrivate               *priv   = plugin->plugin_private;

  if (type == META_COMP_WINDOW_NORMAL)
    {
      ActorPrivate *apriv  = get_actor_private (actor);

      clutter_actor_move_anchor_point_from_gravity (actor,
                                                    CLUTTER_GRAVITY_CENTER);

      plugin->running++;

      apriv->tml_destroy = clutter_effect_scale (priv->destroy_effect,
                                                 actor,
                                                 1.0,
                                                 0.0,
                                                 (ClutterEffectCompleteFunc)
                                                 on_destroy_effect_complete,
                                                 NULL);
    }
  else if (plugin->completed)
    plugin->completed (plugin, actor, META_COMPOSITOR_CLUTTER_PLUGIN_DESTROY);
}

static void
kill_effect (ClutterActor *actor, gulong event)
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;
  ActorPrivate                *apriv;

  if (!(plugin->features & event))
    {
      /* Event we do not support */
      return;
    }

  if (event & META_COMPOSITOR_CLUTTER_PLUGIN_SWITCH_WORKSPACE)
    {
      PluginPrivate *ppriv  = plugin->plugin_private;

      if (ppriv->tml_switch_workspace1)
        {
          clutter_timeline_stop (ppriv->tml_switch_workspace1);
          clutter_timeline_stop (ppriv->tml_switch_workspace2);
          on_switch_workspace_effect_complete (ppriv->desktop1, ppriv->actors);
        }

      if (!(event & ~META_COMPOSITOR_CLUTTER_PLUGIN_SWITCH_WORKSPACE))
        {
          /* Workspace switch only, nothing more to do */
          return;
        }
    }

  apriv = get_actor_private (actor);

  if ((event & META_COMPOSITOR_CLUTTER_PLUGIN_MINIMIZE) && apriv->tml_minimize)
    {
      clutter_timeline_stop (apriv->tml_minimize);
      on_minimize_effect_complete (actor, NULL);
    }

  if ((event & META_COMPOSITOR_CLUTTER_PLUGIN_MAXIMIZE) && apriv->tml_maximize)
    {
      clutter_timeline_stop (apriv->tml_maximize);
      on_maximize_effect_complete (actor, NULL);
    }

  if ((event & META_COMPOSITOR_CLUTTER_PLUGIN_MAP) && apriv->tml_map)
    {
      clutter_timeline_stop (apriv->tml_map);
      on_map_effect_complete (actor, NULL);
    }

  if ((event & META_COMPOSITOR_CLUTTER_PLUGIN_DESTROY) && apriv->tml_destroy)
    {
      clutter_timeline_stop (apriv->tml_destroy);
      on_destroy_effect_complete (actor, NULL);
    }
}


#if 0
const gchar * g_module_check_init (GModule *module);
const gchar *
g_module_check_init (GModule *module)
{
  /*
   * Unused; left here for documentation purposes.
   *
   * NB: this function is called *before* the plugin manager does its own
   *     initialization of the plugin struct, so you cannot process fields
   *     like .params in here; use the init function below instead.
   */
  return NULL;
}
#endif

/*
 * Core of the plugin init function, called for initial initialization and
 * by the reload() function. Returns TRUE on success.
 */
static gboolean
do_init ()
{
  MetaCompositorClutterPlugin *plugin = &META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT;

  PluginPrivate *priv = g_new0 (PluginPrivate, 1);
  const gchar   *params;
  guint          destroy_timeout  = DESTROY_TIMEOUT;
  guint          minimize_timeout = MINIMIZE_TIMEOUT;
  guint          maximize_timeout = MAXIMIZE_TIMEOUT;
  guint          map_timeout      = MAP_TIMEOUT;
  guint          switch_timeout   = SWITCH_TIMEOUT;
  const gchar   *name;

  plugin->plugin_private = priv;

  name = plugin->name;
  plugin->name = _(name);

  params = plugin->params;

  if (params)
    {
      gchar *p;

      if (strstr (params, "debug"))
        {
          g_debug ("%s: Entering debug mode.",
                   META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT.name);

          priv->debug_mode = TRUE;

          /*
           * Double the effect duration to make them easier to observe.
           */
          destroy_timeout  *= 2;
          minimize_timeout *= 2;
          maximize_timeout *= 2;
          map_timeout      *= 2;
          switch_timeout   *= 2;
        }

      if ((p = strstr (params, "disable:")))
        {
          gchar *d = g_strdup (p+8);

          p = strchr (d, ';');

          if (p)
            *p = 0;

          if (strstr (d, "minimize"))
            plugin->features &= ~ META_COMPOSITOR_CLUTTER_PLUGIN_MINIMIZE;

          if (strstr (d, "maximize"))
            plugin->features &= ~ META_COMPOSITOR_CLUTTER_PLUGIN_MAXIMIZE;

          if (strstr (d, "unmaximize"))
            plugin->features &= ~ META_COMPOSITOR_CLUTTER_PLUGIN_UNMAXIMIZE;

          if (strstr (d, "map"))
            plugin->features &= ~ META_COMPOSITOR_CLUTTER_PLUGIN_MAP;

          if (strstr (d, "destroy"))
            plugin->features &= ~ META_COMPOSITOR_CLUTTER_PLUGIN_DESTROY;

          if (strstr (d, "switch-workspace"))
            plugin->features &= ~META_COMPOSITOR_CLUTTER_PLUGIN_SWITCH_WORKSPACE;

          g_free (d);
        }
    }

  priv->destroy_effect
    =  clutter_effect_template_new (clutter_timeline_new_for_duration (
							destroy_timeout),
                                    CLUTTER_ALPHA_SINE_INC);


  priv->minimize_effect
    =  clutter_effect_template_new (clutter_timeline_new_for_duration (
							minimize_timeout),
                                    CLUTTER_ALPHA_SINE_INC);

  priv->maximize_effect
    =  clutter_effect_template_new (clutter_timeline_new_for_duration (
							maximize_timeout),
                                    CLUTTER_ALPHA_SINE_INC);

  priv->map_effect
    =  clutter_effect_template_new (clutter_timeline_new_for_duration (
							map_timeout),
                                    CLUTTER_ALPHA_SINE_INC);

  priv->switch_workspace_effect
    =  clutter_effect_template_new (clutter_timeline_new_for_duration (
							switch_timeout),
                                    CLUTTER_ALPHA_SINE_INC);

  return TRUE;
}

META_COMPOSITOR_CLUTTER_PLUGIN_INIT_FUNC
{
  return do_init ();
}

static void
free_plugin_private (PluginPrivate *priv)
{
  g_object_unref (priv->destroy_effect);
  g_object_unref (priv->minimize_effect);
  g_object_unref (priv->maximize_effect);
  g_object_unref (priv->switch_workspace_effect);

  g_free (priv);

  META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT.plugin_private = NULL;
}

/*
 * Called by the plugin manager when we stuff like the command line parameters
 * changed.
 */
static gboolean
reload ()
{
  PluginPrivate *priv;

  priv = META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT.plugin_private;

  if (do_init ())
    {
      /* Success; free the old private struct */
      free_plugin_private (priv);
      return TRUE;
    }
  else
    {
      /* Fail -- fall back to the old private. */
      META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT.plugin_private = priv;
    }

  return FALSE;
}

/*
 * GModule unload function -- do any cleanup required.
 */
void g_module_unload (GModule *module);
void g_module_unload (GModule *module)
{
  PluginPrivate *priv;

  priv = META_COMPOSITOR_CLUTTER_PLUGIN_STRUCT.plugin_private;

  free_plugin_private (priv);
}