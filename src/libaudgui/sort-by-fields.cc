/*
 * sort-by-fields.cc
 * Copyright 2026 Audacious developers
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

/*
 * "Sort by Custom Order" dialog: lets the user build an ordered chain of
 * fields (artist, album, disc number, track number, ...) to sort the active
 * playlist by, similar to DeadBeef's "Sort by Custom" feature.  Unlike the
 * "Custom Title" sort scheme, this is independent of the generic title
 * format used for display, and unlike a free-text format string, numeric
 * fields are always compared numerically, so there is no need for manual
 * zero-padding.
 */

#include <string.h>

#include <gtk/gtk.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/i18n.h>
#include <libaudcore/playlist.h>
#include <libaudcore/runtime.h>

#include "gtk-compat.h"
#include "internal.h"
#include "libaudgui.h"
#include "libaudgui-gtk.h"

struct FieldInfo
{
    const char * name; // translatable display name
    Tuple::Field field;
};

// fields the user may choose to sort by, in menu order
static const FieldInfo s_fields[] = {
    {N_("Track Number"), Tuple::Track},
    {N_("Title"), Tuple::Title},
    {N_("Artist"), Tuple::Artist},
    {N_("Album"), Tuple::Album},
    {N_("Album Artist"), Tuple::AlbumArtist},
    {N_("Genre"), Tuple::Genre},
    {N_("Date"), Tuple::Date},
    {N_("Length"), Tuple::Length},
    {N_("Comment"), Tuple::Comment},
    {N_("Publisher"), Tuple::Publisher},
    {N_("Catalog Number"), Tuple::CatalogNum},
    {N_("Disc Number"), Tuple::Disc},
    {N_("File Name"), Tuple::Basename},
    {N_("File Path"), Tuple::Path},
    {N_("File Created"), Tuple::FileCreated},
    {N_("File Modified"), Tuple::FileModified},
    {N_("Bitrate"), Tuple::Bitrate}};

static constexpr int n_fields = aud::n_elems(s_fields);

// the default chain offered when no custom order has been saved yet
static const struct
{
    Tuple::Field field;
    bool descending;
} s_default_keys[] = {{Tuple::Artist, false},
                      {Tuple::Album, false},
                      {Tuple::Disc, false},
                      {Tuple::Track, false}};

static int field_to_index(Tuple::Field field)
{
    for (int i = 0; i < n_fields; i++)
    {
        if (s_fields[i].field == field)
            return i;
    }

    return 0;
}

static GtkWidget * create_row(GtkWidget * rows_box, Tuple::Field field,
                              bool descending);

static void add_row_cb(void * rows_box)
{
    GtkWidget * row =
        create_row((GtkWidget *)rows_box, Tuple::Artist, false);
    gtk_widget_show_all(row);
}

static void remove_row_cb(void * row) { gtk_widget_destroy((GtkWidget *)row); }

static void move_row(GtkWidget * row, int delta)
{
    GtkWidget * box = gtk_widget_get_parent(row);
    if (!box)
        return;

    GList * children = gtk_container_get_children((GtkContainer *)box);
    int pos = g_list_index(children, row);
    int n = g_list_length(children);
    g_list_free(children);

    int new_pos = pos + delta;
    if (pos < 0 || new_pos < 0 || new_pos >= n)
        return;

    gtk_box_reorder_child((GtkBox *)box, row, new_pos);
}

static void move_up_cb(void * row) { move_row((GtkWidget *)row, -1); }
static void move_down_cb(void * row) { move_row((GtkWidget *)row, 1); }

static GtkWidget * create_row(GtkWidget * rows_box, Tuple::Field field,
                              bool descending)
{
    GtkWidget * row = audgui_hbox_new(3);

    GtkWidget * combo = gtk_combo_box_text_new();
    for (int i = 0; i < n_fields; i++)
        gtk_combo_box_text_append_text((GtkComboBoxText *)combo, _(s_fields[i].name));
    gtk_combo_box_set_active((GtkComboBox *)combo, field_to_index(field));
    gtk_box_pack_start((GtkBox *)row, combo, true, true, 0);

    GtkWidget * check = gtk_check_button_new_with_mnemonic(_("_Descending"));
    gtk_toggle_button_set_active((GtkToggleButton *)check, descending);
    gtk_box_pack_start((GtkBox *)row, check, false, false, 0);

    GtkWidget * up = audgui_button_new(_("\xE2\x96\xB2"), nullptr, move_up_cb, row);
    gtk_box_pack_start((GtkBox *)row, up, false, false, 0);

    GtkWidget * down = audgui_button_new(_("\xE2\x96\xBC"), nullptr, move_down_cb, row);
    gtk_box_pack_start((GtkBox *)row, down, false, false, 0);

    GtkWidget * remove = audgui_button_new(_("\xC3\x97"), nullptr, remove_row_cb, row);
    gtk_box_pack_start((GtkBox *)row, remove, false, false, 0);

    g_object_set_data((GObject *)row, "combo", combo);
    g_object_set_data((GObject *)row, "check", check);

    gtk_box_pack_start((GtkBox *)rows_box, row, false, false, 0);

    return row;
}

static void collect_keys(GtkWidget * rows_box, Index<Playlist::SortKey> & keys,
                         Index<String> & tokens)
{
    GList * children = gtk_container_get_children((GtkContainer *)rows_box);

    for (GList * node = children; node; node = node->next)
    {
        GtkWidget * row = (GtkWidget *)node->data;
        auto combo =
            (GtkWidget *)g_object_get_data((GObject *)row, "combo");
        auto check =
            (GtkWidget *)g_object_get_data((GObject *)row, "check");

        int idx = gtk_combo_box_get_active((GtkComboBox *)combo);
        if (idx < 0 || idx >= n_fields)
            continue;

        bool descending = gtk_toggle_button_get_active((GtkToggleButton *)check);

        keys.append(Playlist::SortKey{s_fields[idx].field, descending});
        tokens.append(String(str_printf("%s%s", Tuple::field_get_name(s_fields[idx].field),
                                        descending ? ":desc" : "")));
    }

    g_list_free(children);
}

static void sort_cb(void * dialog_)
{
    GtkWidget * dialog = (GtkWidget *)dialog_;
    auto rows_box = (GtkWidget *)g_object_get_data((GObject *)dialog, "rows-box");
    bool selected_only =
        GPOINTER_TO_INT(g_object_get_data((GObject *)dialog, "selected-only"));

    Index<Playlist::SortKey> keys;
    Index<String> tokens;
    collect_keys(rows_box, keys, tokens);

    aud_set_str("gtkui", "custom_sort_keys", index_to_str_list(tokens, ","));

    if (keys.len())
    {
        auto list = Playlist::active_playlist();
        if (selected_only)
            list.sort_selected_by_keys(keys);
        else
            list.sort_by_keys(keys);
    }

    gtk_widget_destroy(dialog);
}

static void load_keys(GtkWidget * rows_box)
{
    String saved = aud_get_str("gtkui", "custom_sort_keys");
    bool any = false;

    if (saved[0])
    {
        for (const String & token : str_list_to_index(saved, ","))
        {
            bool descending = false;
            StringBuf name = str_copy(token);

            const char * sep = strstr(name, ":desc");
            if (sep && !sep[5])
            {
                descending = true;
                name.resize(sep - (const char *)name);
            }

            Tuple::Field field = Tuple::field_by_name(name);
            if (field != Tuple::Invalid)
            {
                create_row(rows_box, field, descending);
                any = true;
            }
        }
    }

    if (!any)
    {
        for (auto & key : s_default_keys)
            create_row(rows_box, key.field, key.descending);
    }
}

static GtkWidget * create_sort_dialog(bool selected_only)
{
    int dpi = audgui_get_dpi();

    GtkWidget * dialog = gtk_dialog_new();
    gtk_window_set_title((GtkWindow *)dialog, _("Sort by Custom Order"));
    gtk_window_set_role((GtkWindow *)dialog, "sort-by-fields");
    gtk_window_set_default_size((GtkWindow *)dialog, 3 * dpi, 2 * dpi);

    GtkWidget * vbox = gtk_dialog_get_content_area((GtkDialog *)dialog);

    GtkWidget * label = gtk_label_new(
        _("Sort by the following fields, in order:"));
    gtk_misc_set_alignment((GtkMisc *)label, 0, 0.5);
    gtk_box_pack_start((GtkBox *)vbox, label, false, false, 0);

    GtkWidget * scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type((GtkScrolledWindow *)scrolled, GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy((GtkScrolledWindow *)scrolled,
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start((GtkBox *)vbox, scrolled, true, true, 0);

    GtkWidget * rows_box = audgui_vbox_new(3);
    gtk_scrolled_window_add_with_viewport((GtkScrolledWindow *)scrolled, rows_box);

    load_keys(rows_box);

    GtkWidget * add_button =
        audgui_button_new(_("_Add Field"), "list-add", add_row_cb, rows_box);
    GtkWidget * add_box = audgui_hbox_new(0);
    gtk_box_pack_start((GtkBox *)add_box, add_button, false, false, 0);
    gtk_box_pack_start((GtkBox *)vbox, add_box, false, false, 0);

    GtkWidget * sort_button =
        audgui_button_new(_("_Sort"), "view-sort-ascending", sort_cb, dialog);
    GtkWidget * cancel_button = audgui_button_new(
        _("_Cancel"), "process-stop", (AudguiCallback)gtk_widget_destroy, dialog);

    gtk_dialog_add_action_widget((GtkDialog *)dialog, cancel_button, GTK_RESPONSE_NONE);
    gtk_dialog_add_action_widget((GtkDialog *)dialog, sort_button, GTK_RESPONSE_NONE);

    g_object_set_data((GObject *)dialog, "rows-box", rows_box);
    g_object_set_data((GObject *)dialog, "selected-only",
                      GINT_TO_POINTER(selected_only));

    return dialog;
}

EXPORT void audgui_show_sort_by_fields(bool selected_only)
{
    if (audgui_reshow_unique_window(AUDGUI_SORT_BY_FIELDS_WINDOW))
        return;

    audgui_show_unique_window(AUDGUI_SORT_BY_FIELDS_WINDOW,
                              create_sort_dialog(selected_only));
}
