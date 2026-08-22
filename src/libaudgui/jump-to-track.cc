/*
 * jump-to-track.c
 * Copyright 2007-2014 Yoshiki Yazawa, John Lindgren, and Thomas Lange
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/playlist.h>
#include <libaudcore/runtime.h>

#include "gtk-compat.h"
#include "internal.h"
#include "libaudgui.h"
#include "libaudgui-gtk.h"
#include "list.h"
#include "jump-to-track-cache.h"

static void update_cb (void * data, void *);
static void activate_cb (void * data, void *);

static JumpToTrackCache cache;
static const KeywordMatches * search_matches;
static GtkWidget * treeview, * filter_entry, * queue_button, * jump_button, * copy_button;
static bool watching = false;

/* ==== coloring ====
 * Mirrors the color scheme used by the Qt "Jump to Song" dialog: the entry
 * number gets a gradient across the currently filtered rows, and each
 * distinct artist name / sample rate string gets its own hash-based color.
 * Rendered via Pango markup since the generic list widget has no per-cell
 * foreground-color callback. */

static void hsl_to_rgb (double h, double s, double l, guint8 & r, guint8 & g, guint8 & b)
{
    auto hue2rgb = [] (double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 1.0 / 2) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };

    double rf, gf, bf;
    if (s == 0)
        rf = gf = bf = l;
    else
    {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        double p = 2 * l - q;
        rf = hue2rgb (p, q, h + 1.0 / 3);
        gf = hue2rgb (p, q, h);
        bf = hue2rgb (p, q, h - 1.0 / 3);
    }

    r = (guint8) (rf * 255 + 0.5);
    g = (guint8) (gf * 255 + 0.5);
    b = (guint8) (bf * 255 + 0.5);
}

/* Deterministic string hash (FNV-1a); not g_str_hash(), which is not
 * guaranteed stable across GLib versions. */
static guint32 stable_hash (const char * key)
{
    guint32 h = 2166136261u;
    for (const unsigned char * p = (const unsigned char *) key; * p; p ++)
    {
        h ^= * p;
        h *= 16777619u;
    }
    return h;
}

static StringBuf name_hex_color (const char * key)
{
    if (! key || ! key[0])
        return StringBuf ();

    char * lower = g_utf8_strdown (key, -1);
    guint32 h = stable_hash (lower);
    g_free (lower);

    double hue = (h % 360) / 360.0;
    double sat = 0.50 + ((h >> 8) % 100) / 100.0 * 0.30;
    double light = 0.62 + ((h >> 16) % 100) / 100.0 * 0.20;

    guint8 r, g, b;
    hsl_to_rgb (hue, sat, light, r, g, b);
    return str_printf ("#%02x%02x%02x", r, g, b);
}

static StringBuf index_hex_color (int row, int row_count)
{
    double t = row_count > 1 ? (double) row / (row_count - 1) : 0.0;
    double hue = (240.0 - 240.0 * t) / 360.0;

    guint8 r, g, b;
    hsl_to_rgb (hue, 0.65, 0.55, r, g, b);
    return str_printf ("#%02x%02x%02x", r, g, b);
}

/* Formats a sample rate in Hertz as a compact string, e.g. 44100 -> "44.1
 * kHz".  Returns an empty string if <hz> is not a valid rate (this happens
 * when the decoder plugin for a given track doesn't report one). */
static StringBuf format_rate (int hz)
{
    if (hz <= 0)
        return StringBuf ();
    if (hz % 1000 == 0)
        return str_printf ("%d kHz", hz / 1000);
    return str_printf ("%.1f kHz", hz / 1000.0);
}

static StringBuf colored_markup (const char * color, const char * escaped_text)
{
    if (! escaped_text)
        escaped_text = "";
    if (color && color[0])
        return str_printf ("<span foreground=\"%s\">%s</span>", color, escaped_text);
    return str_printf ("%s", escaped_text);
}

static void destroy_cb ()
{
    if (watching)
    {
        hook_dissociate ("playlist update", update_cb);
        hook_dissociate ("playlist activate", activate_cb);
        watching = false;
    }

    cache.clear ();

    search_matches = nullptr;
}

static int get_selected_entry ()
{
    g_return_val_if_fail (treeview && search_matches, -1);

    GtkTreeModel * model = gtk_tree_view_get_model ((GtkTreeView *) treeview);
    GtkTreeSelection * selection = gtk_tree_view_get_selection ((GtkTreeView *) treeview);
    GtkTreeIter iter;

    if (! gtk_tree_selection_get_selected (selection, nullptr, & iter))
        return -1;

    GtkTreePath * path = gtk_tree_model_get_path (model, & iter);
    int row = gtk_tree_path_get_indices (path)[0];
    gtk_tree_path_free (path);

    g_return_val_if_fail (row >= 0 && row < search_matches->len (), -1);
    return (* search_matches)[row].entry;
}

static void do_jump (void *)
{
    int entry = get_selected_entry ();
    if (entry < 0)
        return;

    auto playlist = Playlist::active_playlist ();
    playlist.set_position (entry);
    playlist.start_playback ();

    if (aud_get_bool ("audgui", "close_jtf_dialog"))
        audgui_jump_to_track_hide ();
}

static void update_queue_button (int entry)
{
    g_return_if_fail (queue_button);

    if (entry < 0)
    {
        gtk_button_set_label ((GtkButton *) queue_button, _("_Queue"));
        gtk_widget_set_sensitive (queue_button, false);
    }
    else
    {
        if (Playlist::active_playlist ().queue_find_entry (entry) >= 0)
            gtk_button_set_label ((GtkButton *) queue_button, _("Un_queue"));
        else
            gtk_button_set_label ((GtkButton *) queue_button, _("_Queue"));

        gtk_widget_set_sensitive (queue_button, true);
    }
}

static void do_queue (void *)
{
    auto playlist = Playlist::active_playlist ();
    int entry = get_selected_entry ();
    if (entry < 0)
        return;

    int queued = playlist.queue_find_entry (entry);
    if (queued >= 0)
        playlist.queue_remove (queued);
    else
        playlist.queue_insert (-1, entry);

    update_queue_button (entry);
}

/* Copies every currently filtered row (not just the selection) into
 * <target>, preserving each entry's already-scanned metadata so the copy
 * doesn't have to be rescanned. */
static void copy_matches_to_playlist (Playlist target)
{
    g_return_if_fail (search_matches);

    auto source = Playlist::active_playlist ();
    Index<PlaylistAddItem> items;

    for (const KeywordMatch & match : * search_matches)
    {
        int entry = match.entry;
        items.append (source.entry_filename (entry),
         source.entry_tuple (entry, Playlist::NoWait).ref (),
         source.entry_decoder (entry, Playlist::NoWait));
    }

    target.insert_items (target.n_entries (), std::move (items), false);
}

static void copy_menu_new_cb (GtkMenuItem *, void *)
{
    copy_matches_to_playlist (Playlist::new_playlist ());
}

static void copy_menu_existing_cb (GtkMenuItem *, void * data)
{
    Playlist target = Playlist::by_index (GPOINTER_TO_INT (data));
    if (target.exists ())
        copy_matches_to_playlist (target);
}

static void do_copy (void *)
{
    g_return_if_fail (copy_button);

    GtkWidget * menu = gtk_menu_new ();

    GtkWidget * new_item = gtk_menu_item_new_with_mnemonic (_("_New Playlist"));
    g_signal_connect (new_item, "activate", (GCallback) copy_menu_new_cb, nullptr);
    gtk_menu_shell_append ((GtkMenuShell *) menu, new_item);

    int n_playlists = Playlist::n_playlists ();
    if (n_playlists > 0)
    {
        gtk_menu_shell_append ((GtkMenuShell *) menu, gtk_separator_menu_item_new ());

        for (int i = 0; i < n_playlists; i ++)
        {
            String title = Playlist::by_index (i).get_title ();
            GtkWidget * item = gtk_menu_item_new_with_label (title);
            g_signal_connect (item, "activate", (GCallback) copy_menu_existing_cb,
             GINT_TO_POINTER (i));
            gtk_menu_shell_append ((GtkMenuShell *) menu, item);
        }
    }

    g_signal_connect (menu, "selection-done", (GCallback) gtk_widget_destroy, nullptr);
    gtk_widget_show_all (menu);

#ifdef USE_GTK3
    gtk_menu_popup_at_widget ((GtkMenu *) menu, copy_button,
     GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_SOUTH_WEST, nullptr);
#else
    gtk_menu_popup ((GtkMenu *) menu, nullptr, nullptr, nullptr, nullptr, 0, GDK_CURRENT_TIME);
#endif
}

static void selection_changed ()
{
    int entry = get_selected_entry ();
    gtk_widget_set_sensitive (jump_button, entry >= 0);

    update_queue_button (entry);
}

static gboolean keypress_cb (GtkWidget * widget, GdkEventKey * event)
{
    if (event->keyval == GDK_KEY_Escape)
    {
        audgui_jump_to_track_hide ();
        return true;
    }

    if (event->keyval == GDK_KEY_Return && (event->state & GDK_SHIFT_MASK))
    {
        /* Let Shift+Enter act as another shortcut for the Queue button */
        gtk_widget_activate (queue_button);
        return true;
    }

    return false;
}

static void fill_list ()
{
    g_return_if_fail (treeview && filter_entry);

    const char * text = gtk_entry_get_text ((GtkEntry *) filter_entry);
    const char * icon_name = text[0] ? "edit-clear" : nullptr;
    g_object_set (filter_entry, "secondary-icon-name", icon_name, nullptr);

    search_matches = cache.search (text);

    audgui_list_delete_rows (treeview, 0, audgui_list_row_count (treeview));
    audgui_list_insert_rows (treeview, 0, search_matches->len ());

    if (copy_button)
        gtk_widget_set_sensitive (copy_button, search_matches->len () > 0);

    if (search_matches->len () >= 1)
    {
        GtkTreeSelection * sel = gtk_tree_view_get_selection ((GtkTreeView *) treeview);
        GtkTreePath * path = gtk_tree_path_new_from_indices (0, -1);
        gtk_tree_selection_select_path (sel, path);
        gtk_tree_path_free (path);
    }
}

static void update_cb (void * data, void *)
{
    g_return_if_fail (treeview);

    GtkTreeModel * model;
    GtkTreeIter iter;
    GtkTreePath * path = nullptr;

    auto level = aud::from_ptr<Playlist::UpdateLevel> (data);
    if (level <= Playlist::Selection)
        return;

    cache.clear ();

    /* If it's only a metadata update, save and restore the cursor position. */
    if (level <= Playlist::Metadata &&
     gtk_tree_selection_get_selected (gtk_tree_view_get_selection
     ((GtkTreeView *) treeview), & model, & iter))
        path = gtk_tree_model_get_path (model, & iter);

    fill_list ();

    if (path != nullptr)
    {
        gtk_tree_selection_select_path (gtk_tree_view_get_selection
         ((GtkTreeView *) treeview), path);
        gtk_tree_view_scroll_to_cell ((GtkTreeView *) treeview, path, nullptr, true, 0.5, 0);
        gtk_tree_path_free (path);
    }
}

static void activate_cb (void * data, void *)
{
    update_cb (aud::to_ptr (Playlist::Structure), nullptr);
}

static void filter_icon_cb (GtkEntry * entry)
{
    gtk_entry_set_text (entry, "");
}

static void toggle_button_cb (GtkToggleButton * toggle)
{
    aud_set_bool ("audgui", "close_jtf_dialog", gtk_toggle_button_get_active (toggle));
}

static void list_get_value (void * user, int row, int column, GValue * value)
{
    g_return_if_fail (search_matches);
    g_return_if_fail (column >= 0 && column < 3);
    g_return_if_fail (row >= 0 && row < search_matches->len ());

    const KeywordMatch & match = (* search_matches)[row];
    int entry = match.entry;

    switch (column)
    {
    case 0:
    {
        StringBuf color = index_hex_color (row, search_matches->len ());
        StringBuf num = str_printf ("%d", 1 + entry);
        g_value_set_string (value, colored_markup (color, num));
        break;
    }
    case 1:
    {
        auto playlist = Playlist::active_playlist ();
        Tuple tuple = playlist.entry_tuple (entry, Playlist::NoWait);
        StringBuf rate = format_rate (tuple.get_int (Tuple::SampleRate));
        StringBuf color = name_hex_color (rate);
        g_value_set_string (value, colored_markup (color, rate));
        break;
    }
    case 2:
    {
        auto playlist = Playlist::active_playlist ();
        Tuple tuple = playlist.entry_tuple (entry, Playlist::NoWait);

        String artist = tuple.get_str (Tuple::Artist);
        String album = tuple.get_str (Tuple::Album);
        String title = tuple.get_str (Tuple::Title);
        if (! title || ! title[0])
            title = tuple.get_str (Tuple::FormattedTitle);

        GString * out = g_string_new (nullptr);
        bool need_sep = false;

        auto add_part = [&] (const char * text, StringBuf color) {
            if (! text || ! text[0])
                return;
            if (need_sep)
                g_string_append (out, " - ");
            char * esc = g_markup_escape_text (text, -1);
            if (color.len ())
                g_string_append_printf (out, "<span foreground=\"%s\">%s</span>",
                                        (const char *) color, esc);
            else
                g_string_append (out, esc);
            g_free (esc);
            need_sep = true;
        };

        // Artist and album each get their own hash-based color; the bare
        // track title stays in the default text color.
        add_part (artist, name_hex_color (artist));
        add_part (album, name_hex_color (album));
        add_part (title, StringBuf ());

        g_value_set_string (value, out->str);
        g_string_free (out, true);
        break;
    }
    }
}

static const AudguiListCallbacks callbacks = {
    list_get_value
};

static GtkWidget * create_window ()
{
    int dpi = audgui_get_dpi ();

    GtkWidget * jump_to_track_win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_type_hint ((GtkWindow *) jump_to_track_win, GDK_WINDOW_TYPE_HINT_DIALOG);

    gtk_window_set_title ((GtkWindow *) jump_to_track_win, _("Jump to Song"));
    gtk_window_set_role ((GtkWindow *) jump_to_track_win, "jump-to-song");

    g_signal_connect (jump_to_track_win, "key_press_event", (GCallback) keypress_cb, nullptr);
    g_signal_connect (jump_to_track_win, "destroy", (GCallback) destroy_cb, nullptr);

    gtk_container_set_border_width ((GtkContainer *) jump_to_track_win, 10);
    gtk_window_set_default_size ((GtkWindow *) jump_to_track_win, 6 * dpi, 5 * dpi);

    GtkWidget * vbox = audgui_vbox_new (6);
    gtk_container_add ((GtkContainer *) jump_to_track_win, vbox);

    treeview = audgui_list_new (& callbacks, nullptr, 0);
    gtk_tree_view_set_headers_visible ((GtkTreeView *) treeview, false);

    audgui_list_add_column (treeview, nullptr, 0, G_TYPE_STRING, 7, true);
    audgui_list_add_column (treeview, nullptr, 1, G_TYPE_STRING, 8, true);
    audgui_list_add_column (treeview, nullptr, 2, G_TYPE_STRING, -1, true);

    g_signal_connect (gtk_tree_view_get_selection ((GtkTreeView *) treeview),
     "changed", (GCallback) selection_changed, nullptr);
    g_signal_connect (treeview, "row-activated", (GCallback) do_jump, nullptr);

    GtkWidget * hbox = audgui_hbox_new (6);
    gtk_box_pack_start ((GtkBox *) vbox, hbox, false, false, 3);

    /* filter box */
    GtkWidget * search_label = gtk_label_new_with_mnemonic (_("_Filter:"));
    gtk_box_pack_start ((GtkBox *) hbox, search_label, false, false, 0);

    filter_entry = gtk_entry_new ();
    gtk_label_set_mnemonic_widget ((GtkLabel *) search_label, filter_entry);
    g_signal_connect (filter_entry, "changed", (GCallback) fill_list, nullptr);
    g_signal_connect (filter_entry, "icon-press", (GCallback) filter_icon_cb, nullptr);
    gtk_entry_set_activates_default ((GtkEntry *) filter_entry, true);
    gtk_box_pack_start ((GtkBox *) hbox, filter_entry, true, true, 0);

    GtkWidget * scrollwin = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_container_add ((GtkContainer *) scrollwin, treeview);
    gtk_scrolled_window_set_policy ((GtkScrolledWindow *) scrollwin,
     GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type ((GtkScrolledWindow *) scrollwin, GTK_SHADOW_IN);
    gtk_box_pack_start ((GtkBox *) vbox, scrollwin, true, true, 0);

    GtkWidget * hbox2 = audgui_hbox_new (0);
    gtk_box_pack_end ((GtkBox *) vbox, hbox2, false, false, 0);

    GtkWidget * bbox = audgui_button_box_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout ((GtkButtonBox *) bbox, GTK_BUTTONBOX_END);
    gtk_box_set_spacing ((GtkBox *) bbox, 6);

#ifdef USE_GTK3
    gtk_widget_set_margin_start (bbox, 6);
    gtk_box_pack_end ((GtkBox *) hbox2, bbox, true, true, 0);
#else
    GtkWidget * alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
    gtk_alignment_set_padding ((GtkAlignment *) alignment, 0, 0, 6, 0);
    gtk_container_add ((GtkContainer *) alignment, bbox);
    gtk_box_pack_end ((GtkBox *) hbox2, alignment, true, true, 0);
#endif

    /* close dialog toggle */
    GtkWidget * toggle = gtk_check_button_new_with_mnemonic (_("C_lose on jump"));
    gtk_toggle_button_set_active ((GtkToggleButton *) toggle, aud_get_bool
     ("audgui", "close_jtf_dialog"));
    gtk_container_add ((GtkContainer *) hbox2, toggle);
    g_signal_connect (toggle, "clicked", (GCallback) toggle_button_cb, nullptr);

    /* queue button */
    queue_button = audgui_button_new (_("_Queue"), nullptr, do_queue, nullptr);
    gtk_container_add ((GtkContainer *) bbox, queue_button);

    /* copy to playlist button */
    copy_button = audgui_button_new (_("Cop_y to Playlist"), nullptr, do_copy, nullptr);
    gtk_container_add ((GtkContainer *) bbox, copy_button);

    /* close button */
    GtkWidget * close = audgui_button_new (_("_Close"), "window-close",
     (AudguiCallback) audgui_jump_to_track_hide, nullptr);
    gtk_container_add ((GtkContainer *) bbox, close);

    /* jump button */
    jump_button = audgui_button_new (_("_Jump"), "go-jump", do_jump, nullptr);
    gtk_container_add ((GtkContainer *) bbox, jump_button);
    gtk_widget_set_can_default (jump_button, true);
    gtk_widget_grab_default (jump_button);

    return jump_to_track_win;
}

EXPORT void audgui_jump_to_track ()
{
    if (audgui_reshow_unique_window (AUDGUI_JUMP_TO_TRACK_WINDOW))
        return;

    GtkWidget * jump_to_track_win = create_window ();

    if (! watching)
    {
        fill_list ();
        hook_associate ("playlist update", update_cb, nullptr);
        hook_associate ("playlist activate", activate_cb, nullptr);
        watching = true;
    }

    gtk_widget_grab_focus (filter_entry);

    audgui_show_unique_window (AUDGUI_JUMP_TO_TRACK_WINDOW, jump_to_track_win);
}

EXPORT void audgui_jump_to_track_hide ()
{
    audgui_hide_unique_window (AUDGUI_JUMP_TO_TRACK_WINDOW);
}
