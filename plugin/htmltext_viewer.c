/*
 * htmltext_viewer.c - Claws Mail plugin: HTML to plain text viewer
 *
 * Renders text/html MIME parts by piping the raw HTML through an
 * external binary (htmltext-render) and displaying the resulting
 * plain text in a GtkTextView.  No network access, no JS, no images.
 *
 * Copyright (C) 2026  <you>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#  include "claws-features.h"
#endif

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

/* Claws Mail public headers */
#include "common/claws.h"
#include "common/version.h"
#include "plugin.h"
#include "mimeview.h"
#include "procmime.h"
#include "utils.h"
#include "file-utils.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define PLUGIN_NAME  (_("HTMLText Viewer"))

/*
 * Name (or full path) of the companion Rust binary.
 * At runtime we search PATH first; a compile-time override can be
 * supplied via -DHTMLTEXT_RENDER_BIN=\"/usr/lib/claws-mail/htmltext-render\".
 */
#ifndef HTMLTEXT_RENDER_BIN
#  define HTMLTEXT_RENDER_BIN "htmltext-render"
#endif

/* Maximum bytes we read from the render process stdout (4 MiB). */
#define MAX_OUTPUT_BYTES (4 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Viewer struct                                                        */
/* ------------------------------------------------------------------ */

typedef struct _HtmlTextViewer HtmlTextViewer;

struct _HtmlTextViewer {
    MimeViewer  mimeviewer;   /* MUST be first – Claws casts the pointer */

    GtkWidget  *scrolled;     /* GtkScrolledWindow (the widget Claws embeds) */
    GtkWidget  *textview;     /* GtkTextView inside the scrolled window      */

    gchar      *tmp_filename; /* temp file holding the current MIME part     */
};

/* Forward declaration */
static MimeViewerFactory htmltext_viewer_factory;

/* ------------------------------------------------------------------ */
/* Helper: spawn htmltext-render, pipe HTML in, collect text out       */
/* ------------------------------------------------------------------ */

/*
 * render_html_to_text:
 *   @html_bytes:  raw HTML content (not NUL-terminated required, but safe)
 *   @html_len:    byte length of @html_bytes
 *   @out_text:    (out) newly allocated NUL-terminated UTF-8 text, or NULL
 *   @out_len:     (out) byte length of *out_text
 *
 * Returns TRUE on success.
 *
 * The child process receives HTML on stdin and must write plain text to
 * stdout.  stderr is inherited so errors surface in the terminal.
 *
 * Security properties:
 *   - The child binary performs no network I/O; it only reads stdin.
 *   - We pass no environment variables beyond the minimal inherited set.
 *   - stdout is collected in memory; we enforce MAX_OUTPUT_BYTES.
 */
static gboolean render_html_to_text(const gchar *html_bytes,
                                    gsize        html_len,
                                    gchar      **out_text,
                                    gsize       *out_len)
{
    gchar  *argv[2] = { HTMLTEXT_RENDER_BIN, NULL };
    gint    stdin_fd  = -1;
    gint    stdout_fd = -1;
    GPid    child_pid = 0;
    GError *err       = NULL;
    gboolean ok       = FALSE;

    *out_text = NULL;
    *out_len  = 0;

    /* Spawn with explicit stdin/stdout pipes; stderr is inherited. */
    if (!g_spawn_async_with_pipes(
            NULL,        /* working directory: inherit */
            argv,
            NULL,        /* environment:       inherit */
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
            NULL, NULL,  /* child_setup, user_data */
            &child_pid,
            &stdin_fd,
            &stdout_fd,
            NULL,        /* stderr: inherit */
            &err)) {
        g_warning(PLUGIN_NAME ": g_spawn_async_with_pipes failed: %s",
                  err ? err->message : "unknown");
        if (err) g_error_free(err);
        return FALSE;
    }

    /* Write HTML to child stdin.
     * We use blocking writes; the HTML is already in memory so this
     * completes quickly.  The child must not block on stdout before
     * consuming all stdin, which is true for htmltext-render (it reads
     * all of stdin then writes stdout). */
    {
        const gchar *p   = html_bytes;
        gsize        rem = html_len;

        while (rem > 0) {
            gssize written = write(stdin_fd, p, rem);
            if (written < 0) {
                if (errno == EINTR) continue;
                g_warning(PLUGIN_NAME ": write to child stdin failed: %s",
                          g_strerror(errno));
                goto cleanup;
            }
            p   += (gsize) written;
            rem -= (gsize) written;
        }
    }
    close(stdin_fd);
    stdin_fd = -1;  /* signal: already closed */

    /* Read child stdout into a GString, capped at MAX_OUTPUT_BYTES. */
    {
        GString *buf = g_string_new_len(NULL, 8192);
        gchar    chunk[4096];

        while (TRUE) {
            gssize n = read(stdout_fd, chunk, sizeof(chunk));
            if (n < 0) {
                if (errno == EINTR) continue;
                g_warning(PLUGIN_NAME ": read from child stdout failed: %s",
                          g_strerror(errno));
                g_string_free(buf, TRUE);
                goto cleanup;
            }
            if (n == 0) break; /* EOF */

            if (buf->len + (gsize) n > MAX_OUTPUT_BYTES) {
                g_warning(PLUGIN_NAME ": output exceeds %d bytes, truncating",
                          MAX_OUTPUT_BYTES);
                g_string_append_len(buf, chunk, (gssize)(MAX_OUTPUT_BYTES - buf->len));
                break;
            }
            g_string_append_len(buf, chunk, n);
        }

        *out_len  = buf->len;
        *out_text = g_string_free(buf, FALSE); /* transfer ownership */
    }

    ok = TRUE;

cleanup:
    if (stdin_fd  >= 0) close(stdin_fd);
    if (stdout_fd >= 0) close(stdout_fd);

    /* Reap the child to avoid zombies. */
    if (child_pid > 0) {
        gint exit_status = 0;
        g_spawn_close_pid(child_pid);
        waitpid(child_pid, &exit_status, 0);
        if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0)
            g_warning(PLUGIN_NAME ": render binary exited with status %d",
                      exit_status);
    }

    return ok;
}

/* ------------------------------------------------------------------ */
/* MimeViewer callbacks                                                */
/* ------------------------------------------------------------------ */

static GtkWidget *htmltext_get_widget(MimeViewer *_viewer)
{
    HtmlTextViewer *viewer = (HtmlTextViewer *) _viewer;
    return GTK_WIDGET(viewer->scrolled);
}

static void htmltext_show_mimepart(MimeViewer *_viewer,
                                   const gchar *infile,
                                   MimeInfo    *partinfo)
{
    HtmlTextViewer *viewer  = (HtmlTextViewer *) _viewer;
    GtkTextBuffer  *buf     = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->textview));
    gchar          *html_content = NULL;
    gsize           html_len     = 0;
    gchar          *plain_text   = NULL;
    gsize           plain_len    = 0;
    GError         *err          = NULL;

    /* Clean up any previous temp file. */
    if (viewer->tmp_filename) {
        claws_unlink(viewer->tmp_filename);
        g_free(viewer->tmp_filename);
        viewer->tmp_filename = NULL;
    }

    /* Extract the MIME part to a temp file then read it back.
     * procmime_get_part writes decoded (transfer-encoding removed) bytes. */
    viewer->tmp_filename = procmime_get_tmp_file_name(partinfo);
    if (!viewer->tmp_filename) {
        gtk_text_buffer_set_text(buf, _("[Error: could not create temp file]"), -1);
        return;
    }

    if (procmime_get_part(viewer->tmp_filename, partinfo) < 0) {
        gtk_text_buffer_set_text(buf, _("[Error: could not extract MIME part]"), -1);
        return;
    }

    if (!g_file_get_contents(viewer->tmp_filename, &html_content, &html_len, &err)) {
        gchar *msg = g_strdup_printf(_("[Error reading temp file: %s]"),
                                     err ? err->message : "?");
        gtk_text_buffer_set_text(buf, msg, -1);
        g_free(msg);
        if (err) g_error_free(err);
        return;
    }

    /* Hand off to the Rust renderer. */
    if (!render_html_to_text(html_content, html_len, &plain_text, &plain_len)) {
        gtk_text_buffer_set_text(buf,
            _("[Error: htmltext-render failed — is it installed and in PATH?]"), -1);
        g_free(html_content);
        return;
    }
    g_free(html_content);

    /* Ensure the text is valid UTF-8 before handing to GTK. */
    if (!g_utf8_validate(plain_text, (gssize) plain_len, NULL)) {
        gchar *safe = g_utf8_make_valid(plain_text, (gssize) plain_len);
        g_free(plain_text);
        plain_text = safe;
        plain_len  = strlen(safe);
    }

    gtk_text_buffer_set_text(buf, plain_text, (gint) plain_len);
    g_free(plain_text);

    /* Scroll to the top. */
    {
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(viewer->textview),
                                     &start, 0.0, FALSE, 0.0, 0.0);
    }
}

static void htmltext_clear_viewer(MimeViewer *_viewer)
{
    HtmlTextViewer *viewer = (HtmlTextViewer *) _viewer;
    GtkTextBuffer  *buf    = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->textview));

    gtk_text_buffer_set_text(buf, "", 0);

    if (viewer->tmp_filename) {
        claws_unlink(viewer->tmp_filename);
        g_free(viewer->tmp_filename);
        viewer->tmp_filename = NULL;
    }
}

static void htmltext_destroy_viewer(MimeViewer *_viewer)
{
    HtmlTextViewer *viewer = (HtmlTextViewer *) _viewer;

    if (viewer->tmp_filename) {
        claws_unlink(viewer->tmp_filename);
        g_free(viewer->tmp_filename);
    }

    g_object_unref(viewer->scrolled);
    g_free(viewer);
}

/* ------------------------------------------------------------------ */
/* Factory                                                             */
/* ------------------------------------------------------------------ */

static MimeViewer *htmltext_viewer_create(void)
{
    HtmlTextViewer *viewer;
    GtkWidget      *textview;
    GtkWidget      *scrolled;

    viewer = g_new0(HtmlTextViewer, 1);

    /* Wire up the MimeViewer vtable. */
    viewer->mimeviewer.factory        = &htmltext_viewer_factory;
    viewer->mimeviewer.get_widget     = htmltext_get_widget;
    viewer->mimeviewer.show_mimepart  = htmltext_show_mimepart;
    viewer->mimeviewer.clear_viewer   = htmltext_clear_viewer;
    viewer->mimeviewer.destroy_viewer = htmltext_destroy_viewer;

    /* Build the GTK widget hierarchy:
     *   GtkScrolledWindow
     *     └─ GtkTextView  (read-only, word-wrapped, monospace) */
    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textview), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textview), 6);

    gtk_container_add(GTK_CONTAINER(scrolled), textview);
    gtk_widget_show_all(scrolled);

    /* Keep a ref so destroy_viewer can unref safely. */
    g_object_ref(scrolled);

    viewer->scrolled  = scrolled;
    viewer->textview  = textview;
    viewer->tmp_filename = NULL;

    return (MimeViewer *) viewer;
}

static gchar *content_types[] = { "text/html", NULL };

static MimeViewerFactory htmltext_viewer_factory = {
    content_types,
    0,
    htmltext_viewer_create,
};

/* ------------------------------------------------------------------ */
/* Plugin entry points (symbols loaded by Claws at runtime)            */
/* ------------------------------------------------------------------ */

gint plugin_init(gchar **error)
{
    if (!check_plugin_version(MAKE_NUMERIC_VERSION(3, 17, 0, 0),
                              VERSION_NUMERIC, PLUGIN_NAME, error))
        return -1;

    mimeview_register_viewer_factory(&htmltext_viewer_factory);

    debug_print(PLUGIN_NAME ": registered viewer factory for text/html\n");
    return 0;
}

gboolean plugin_done(void)
{
    mimeview_unregister_viewer_factory(&htmltext_viewer_factory);
    return TRUE;
}

const gchar *plugin_name(void)    { return PLUGIN_NAME; }
const gchar *plugin_type(void)    { return "GTK3"; }
const gchar *plugin_licence(void) { return "GPL3+"; }
const gchar *plugin_version(void) { return "0.1.0"; }

const gchar *plugin_desc(void)
{
    return _(
        "Renders HTML e-mail parts as plain text by piping the raw HTML "
        "through the 'htmltext-render' helper binary.\n"
        "\n"
        "No JavaScript, no images, no remote resources are fetched.\n"
        "Install 'htmltext-render' somewhere in your PATH.");
}

struct PluginFeature *plugin_provides(void)
{
    static struct PluginFeature features[] = {
        { PLUGIN_MIMEVIEWER, "text/html" },
        { PLUGIN_NOTHING,    NULL        }
    };
    return features;
}
