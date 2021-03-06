/*
 * table.c - functions handling the data at the table level
 *
 * Copyright (C) 2010-2014 Karel Zak <kzak@redhat.com>
 * Copyright (C) 2016 Igor Gnatenko <i.gnatenko.brain@gmail.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

/**
 * SECTION: table_print
 * @title: Table print
 * @short_description: output functions
 *
 * Table output API.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <ctype.h>

#include "mbsalign.h"
#include "carefulputc.h"
#include "smartcolsP.h"

/* Fallback for symbols
 *
 * Note that by default library define all the symbols, but in case user does
 * not define all symbols or if we extended the symbols struct then we need
 * fallback to be more robust and backwardly compatible.
 */
#define titlepadding_symbol(tb)	((tb)->symbols->title_padding ? (tb)->symbols->title_padding : " ")
#define branch_symbol(tb)	((tb)->symbols->tree_branch ? (tb)->symbols->tree_branch : "|-")
#define vertical_symbol(tb)	((tb)->symbols->tree_vert ? (tb)->symbols->tree_vert : "| ")
#define right_symbol(tb)	((tb)->symbols->tree_right ? (tb)->symbols->tree_right : "`-")

#define grp_vertical_symbol(tb)	((tb)->symbols->group_vert ? (tb)->symbols->group_vert : "|")
#define grp_horizontal_symbol(tb) ((tb)->symbols->group_horz ? (tb)->symbols->group_horz : "-")
#define grp_m_first_symbol(tb)	((tb)->symbols->group_first_member ? (tb)->symbols->group_first_member : ",->")
#define grp_m_last_symbol(tb)	((tb)->symbols->group_last_member ? (tb)->symbols->group_last_member : "\\->")
#define grp_m_middle_symbol(tb)	((tb)->symbols->group_middle_member ? (tb)->symbols->group_middle_member : "|->")
#define grp_c_middle_symbol(tb)	((tb)->symbols->group_middle_child ? (tb)->symbols->group_middle_child : "|-")
#define grp_c_last_symbol(tb)	((tb)->symbols->group_last_child ? (tb)->symbols->group_last_child : "`-")

#define cellpadding_symbol(tb)  ((tb)->padding_debug ? "." : \
				 ((tb)->symbols->cell_padding ? (tb)->symbols->cell_padding: " "))

#define want_repeat_header(tb)	(!(tb)->header_repeat || (tb)->header_next <= (tb)->termlines_used)

static int is_next_columns_empty(
			struct libscols_table *tb,
			struct libscols_column *cl,
			struct libscols_line *ln)
{
	struct libscols_iter itr;

	if (!tb || !cl)
		return 0;
	if (is_last_column(cl))
		return 1;
	if (!ln)
		return 0;

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	scols_table_set_columns_iter(tb, &itr, cl);

	/* skip current column */
	scols_table_next_column(tb, &itr, &cl);

	while (scols_table_next_column(tb, &itr, &cl) == 0) {
		struct libscols_cell *ce;
		const char *data = NULL;

                if (scols_column_is_hidden(cl))
                        continue;
		if (scols_column_is_tree(cl))
			return 0;

		ce = scols_line_get_cell(ln, cl->seqnum);
		if (ce)
			data = scols_cell_get_data(ce);
		if (data && *data)
			return 0;
	}
	return 1;
}

/* returns pointer to the end of used data */
static int tree_ascii_art_to_buffer(struct libscols_table *tb,
				    struct libscols_line *ln,
				    struct libscols_buffer *buf)
{
	const char *art;
	int rc;

	assert(ln);
	assert(buf);

	if (!ln->parent)
		return 0;

	rc = tree_ascii_art_to_buffer(tb, ln->parent, buf);
	if (rc)
		return rc;

	if (is_last_child(ln))
		art = "  ";
	else
		art = vertical_symbol(tb);

	return buffer_append_data(buf, art);
}

static int grpset_is_empty(	struct libscols_table *tb,
				size_t idx,
				size_t *rest)
{
	size_t i;

	for (i = idx; i < tb->grpset_size; i++) {
		if (tb->grpset[i] == NULL) {
			if (rest)
				(*rest)++;
		} else
			return 0;
	}
	return 1;
}

static int groups_ascii_art_to_buffer(	struct libscols_table *tb,
				struct libscols_line *ln,
				struct libscols_buffer *buf)
{
	int filled = 0;
	size_t i, rest = 0;
	const char *filler = cellpadding_symbol(tb);

	if (!has_groups(tb))
		return 0;

	DBG(LINE, ul_debugobj(ln, "printing groups chart"));

	if (tb->is_dummy_print)
		return 0;		/* allocate grpset[] only */

	for (i = 0; i < tb->grpset_size; i += SCOLS_GRPSET_CHUNKSIZ) {
		struct libscols_group *gr = tb->grpset[i];

		if (!gr) {
			buffer_append_ntimes(buf, SCOLS_GRPSET_CHUNKSIZ, cellpadding_symbol(tb));
			continue;
		}

		switch (gr->state) {
		case SCOLS_GSTATE_FIRST_MEMBER:
			buffer_append_data(buf, grp_m_first_symbol(tb));
			break;
		case SCOLS_GSTATE_MIDDLE_MEMBER:
			buffer_append_data(buf, grp_m_middle_symbol(tb));
			break;
		case SCOLS_GSTATE_LAST_MEMBER:
			buffer_append_data(buf, grp_m_last_symbol(tb));
			break;
		case SCOLS_GSTATE_CONT_MEMBERS:
			buffer_append_data(buf, grp_vertical_symbol(tb));
			buffer_append_ntimes(buf, 2, filler);
			break;
		case SCOLS_GSTATE_MIDDLE_CHILD:
			buffer_append_data(buf, filler);
			buffer_append_data(buf, grp_c_middle_symbol(tb));
			if (grpset_is_empty(tb, i + SCOLS_GRPSET_CHUNKSIZ, &rest)) {
				buffer_append_ntimes(buf, rest+1, grp_horizontal_symbol(tb));
				filled = 1;
			}
			filler = grp_horizontal_symbol(tb);
			break;
		case SCOLS_GSTATE_LAST_CHILD:
			buffer_append_data(buf, cellpadding_symbol(tb));
			buffer_append_data(buf, grp_c_last_symbol(tb));
			if (grpset_is_empty(tb, i + SCOLS_GRPSET_CHUNKSIZ, &rest)) {
				buffer_append_ntimes(buf, rest+1, grp_horizontal_symbol(tb));
				filled = 1;
			}
			filler = grp_horizontal_symbol(tb);
			break;
		case SCOLS_GSTATE_CONT_CHILDREN:
			buffer_append_data(buf, filler);
			buffer_append_data(buf, grp_vertical_symbol(tb));
			buffer_append_data(buf, filler);
			break;
		}

		if (filled)
			break;
	}

	if (!filled)
		buffer_append_data(buf, filler);
	return 0;
}

static int has_pending_data(struct libscols_table *tb)
{
	struct libscols_column *cl;
	struct libscols_iter itr;

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_column(tb, &itr, &cl) == 0) {
		if (scols_column_is_hidden(cl))
			continue;
		if (cl->pending_data)
			return 1;
	}
	return 0;
}

/* print padding or ASCII-art instead of data of @cl */
static void print_empty_cell(struct libscols_table *tb,
			  struct libscols_column *cl,
			  struct libscols_line *ln,	/* optional */
			  size_t bufsz)
{
	size_t len_pad = 0;		/* in screen cells as opposed to bytes */

	DBG(COL, ul_debugobj(cl, " printing empty cell"));

	/* generate tree ASCII-art rather than padding */
	if (ln && scols_column_is_tree(cl)) {
		if (!ln->parent) {
			/* only print symbols->vert if followed by child */
			if (!list_empty(&ln->ln_branch)) {
				fputs(vertical_symbol(tb), tb->out);
				len_pad = scols_table_is_noencoding(tb) ?
						mbs_width(vertical_symbol(tb)) :
						mbs_safe_width(vertical_symbol(tb));
			}
		} else {
			/* use the same draw function as though we were intending to draw an L-shape */
			struct libscols_buffer *art = new_buffer(bufsz);
			char *data;

			if (art) {
				/* whatever the rc, len_pad will be sensible */
				tree_ascii_art_to_buffer(tb, ln, art);
				if (!list_empty(&ln->ln_branch) && has_pending_data(tb))
					buffer_append_data(art, vertical_symbol(tb));
				data = buffer_get_safe_data(tb, art, &len_pad, NULL);
				if (data && len_pad)
					fputs(data, tb->out);
				free_buffer(art);
			}
		}
	}

	/* minout -- don't fill */
	if (scols_table_is_minout(tb) && is_next_columns_empty(tb, cl, ln))
		return;

	/* default -- fill except last column */
	if (!scols_table_is_maxout(tb) && is_last_column(cl))
		return;

	/* fill rest of cell with space */
	for(; len_pad < cl->width; ++len_pad)
		fputs(cellpadding_symbol(tb), tb->out);

	if (!is_last_column(cl))
		fputs(colsep(tb), tb->out);
}


static const char *get_cell_color(struct libscols_table *tb,
				  struct libscols_column *cl,
				  struct libscols_line *ln,	/* optional */
				  struct libscols_cell *ce)	/* optional */
{
	const char *color = NULL;

	if (tb && tb->colors_wanted) {
		if (ce)
			color = ce->color;
		if (ln && !color)
			color = ln->color;
		if (!color)
			color = cl->color;
	}
	return color;
}

/* Fill the start of a line with padding (or with tree ascii-art).
 *
 * This is necessary after a long non-truncated column, as this requires the
 * next column to be printed on the next line. For example (see 'DDD'):
 *
 * aaa bbb ccc ddd eee
 * AAA BBB CCCCCCC
 *             DDD EEE
 * ^^^^^^^^^^^^
 *  new line padding
 */
static void print_newline_padding(struct libscols_table *tb,
				  struct libscols_column *cl,
				  struct libscols_line *ln,	/* optional */
				  size_t bufsz)
{
	size_t i;

	assert(tb);
	assert(cl);

	DBG(LINE, ul_debugobj(ln, "printing newline padding"));

	fputs(linesep(tb), tb->out);		/* line break */
	tb->termlines_used++;

	/* fill cells after line break */
	for (i = 0; i <= (size_t) cl->seqnum; i++)
		print_empty_cell(tb, scols_table_get_column(tb, i), ln, bufsz);
}

/*
 * Pending data
 *
 * The first line in the multi-line cells (columns with SCOLS_FL_WRAP flag) is
 * printed as usually and output is truncated to match column width.
 *
 * The rest of the long text is printed on next extra line(s). The extra lines
 * don't exist in the table (not represented by libscols_line). The data for
 * the extra lines are stored in libscols_column->pending_data_buf and the
 * function print_line() adds extra lines until the buffer is not empty in all
 * columns.
 */

/* set data that will be printed by extra lines */
static int set_pending_data(struct libscols_column *cl, const char *data, size_t sz)
{
	char *p = NULL;

	if (data && *data) {
		DBG(COL, ul_debugobj(cl, "setting pending data"));
		assert(sz);
		p = strdup(data);
		if (!p)
			return -ENOMEM;
	}

	free(cl->pending_data_buf);
	cl->pending_data_buf = p;
	cl->pending_data_sz = sz;
	cl->pending_data = cl->pending_data_buf;
	return 0;
}

/* the next extra line has been printed, move pending data cursor */
static int step_pending_data(struct libscols_column *cl, size_t bytes)
{
	DBG(COL, ul_debugobj(cl, "step pending data %zu -= %zu", cl->pending_data_sz, bytes));

	if (bytes >= cl->pending_data_sz)
		return set_pending_data(cl, NULL, 0);

	cl->pending_data += bytes;
	cl->pending_data_sz -= bytes;
	return 0;
}

/* print next pending data for the column @cl */
static int print_pending_data(
		struct libscols_table *tb,
		struct libscols_column *cl,
		struct libscols_line *ln,	/* optional */
		struct libscols_cell *ce)
{
	const char *color = get_cell_color(tb, cl, ln, ce);
	size_t width = cl->width, bytes;
	size_t len = width, i;
	char *data;
	char *nextchunk = NULL;

	if (!cl->pending_data)
		return 0;
	if (!width)
		return -EINVAL;

	DBG(COL, ul_debugobj(cl, "printing pending data"));

	data = strdup(cl->pending_data);
	if (!data)
		goto err;

	if (scols_column_is_customwrap(cl)
	    && (nextchunk = cl->wrap_nextchunk(cl, data, cl->wrapfunc_data))) {
		bytes = nextchunk - data;

		len = scols_table_is_noencoding(tb) ?
				mbs_nwidth(data, bytes) :
				mbs_safe_nwidth(data, bytes, NULL);
	} else
		bytes = mbs_truncate(data, &len);

	if (bytes == (size_t) -1)
		goto err;

	if (bytes)
		step_pending_data(cl, bytes);

	if (color)
		fputs(color, tb->out);
	fputs(data, tb->out);
	if (color)
		fputs(UL_COLOR_RESET, tb->out);
	free(data);

	/* minout -- don't fill */
	if (scols_table_is_minout(tb) && is_next_columns_empty(tb, cl, ln))
		return 0;

	/* default -- fill except last column */
	if (!scols_table_is_maxout(tb) && is_last_column(cl))
		return 0;

	/* fill rest of cell with space */
	for(i = len; i < width; i++)
		fputs(cellpadding_symbol(tb), tb->out);

	if (!is_last_column(cl))
		fputs(colsep(tb), tb->out);

	return 0;
err:
	free(data);
	return -errno;
}

static void print_json_data(struct libscols_table *tb,
			    struct libscols_column *cl,
			    const char *name,
			    char *data,
			    int is_last)
{
	switch (cl->json_type) {
	case SCOLS_JSON_STRING:
		/* name: "aaa" */
		ul_jsonwrt_value_s(&tb->json, name, data, is_last);
		break;
	case SCOLS_JSON_NUMBER:
		/* name: 123 */
		ul_jsonwrt_value_raw(&tb->json, name, data, is_last);
		break;
	case SCOLS_JSON_BOOLEAN:
		/* name: true|false */
		ul_jsonwrt_value_boolean(&tb->json, name,
			!*data ? 0 :
			*data == '0' ? 0 :
			*data == 'N' || *data == 'n' ? 0 : 1,
			is_last);
		break;
	case SCOLS_JSON_ARRAY_STRING:
	case SCOLS_JSON_ARRAY_NUMBER:
		/* name: [ "aaa", "bbb", "ccc" ] */
		ul_jsonwrt_array_open(&tb->json, name);

		if (!scols_column_is_customwrap(cl))
			ul_jsonwrt_value_s(&tb->json, NULL, data, 1);
		else do {
				char *next = cl->wrap_nextchunk(cl, data, cl->wrapfunc_data);

				if (cl->json_type == SCOLS_JSON_ARRAY_STRING)
					ul_jsonwrt_value_s(&tb->json, NULL, data, next ? 0 : 1);
				else
					ul_jsonwrt_value_raw(&tb->json, NULL, data, next ? 0 : 1);
				data = next;
		} while (data);

		ul_jsonwrt_array_close(&tb->json, is_last);
		break;
	}
}

static int print_data(struct libscols_table *tb,
		      struct libscols_column *cl,
		      struct libscols_line *ln,	/* optional */
		      struct libscols_cell *ce,	/* optional */
		      struct libscols_buffer *buf)
{
	size_t len = 0, i, width, bytes;
	const char *color = NULL;
	char *data, *nextchunk;
	const char *name = NULL;
	int is_last;

	assert(tb);
	assert(cl);

	data = buffer_get_data(buf);
	if (!data)
		data = "";

	if (tb->format != SCOLS_FMT_HUMAN)
		name = scols_cell_get_data(&cl->header);

	is_last = is_last_column(cl);

	if (is_last && scols_table_is_json(tb) &&
	    scols_table_is_tree(tb) && has_children(ln))
		/* "children": [] is the real last value */
		is_last = 0;

	switch (tb->format) {
	case SCOLS_FMT_RAW:
		fputs_nonblank(data, tb->out);
		if (!is_last)
			fputs(colsep(tb), tb->out);
		return 0;

	case SCOLS_FMT_EXPORT:
		fputs_shell_ident(name, tb->out);
		if (endswith(name, "%"))
			fputs("PCT", tb->out);
		fputc('=', tb->out);
		fputs_quoted(data, tb->out);
		if (!is_last)
			fputs(colsep(tb), tb->out);
		return 0;

	case SCOLS_FMT_JSON:
		print_json_data(tb, cl, name, data, is_last);
		return 0;

	case SCOLS_FMT_HUMAN:
		break;		/* continue below */
	}

	color = get_cell_color(tb, cl, ln, ce);

	/* Encode. Note that 'len' and 'width' are number of cells, not bytes.
	 */
	data = buffer_get_safe_data(tb, buf, &len, scols_column_get_safechars(cl));
	if (!data)
		data = "";
	bytes = strlen(data);
	width = cl->width;

	/* custom multi-line cell based */
	if (*data && scols_column_is_customwrap(cl)
	    && (nextchunk = cl->wrap_nextchunk(cl, data, cl->wrapfunc_data))) {
		set_pending_data(cl, nextchunk, bytes - (nextchunk - data));
		bytes = nextchunk - data;

		len = scols_table_is_noencoding(tb) ?
				mbs_nwidth(data, bytes) :
				mbs_safe_nwidth(data, bytes, NULL);
	}

	if (is_last
	    && len < width
	    && !scols_table_is_maxout(tb)
	    && !scols_column_is_right(cl))
		width = len;

	/* truncate data */
	if (len > width && scols_column_is_trunc(cl)) {
		len = width;
		bytes = mbs_truncate(data, &len);	/* updates 'len' */
	}

	/* standard multi-line cell */
	if (len > width && scols_column_is_wrap(cl)
	    && !scols_column_is_customwrap(cl)) {
		set_pending_data(cl, data, bytes);

		len = width;
		bytes = mbs_truncate(data, &len);
		if (bytes  != (size_t) -1 && bytes > 0)
			step_pending_data(cl, bytes);
	}

	if (bytes == (size_t) -1) {
		bytes = len = 0;
		data = NULL;
	}

	if (data && *data) {
		if (scols_column_is_right(cl)) {
			if (color)
				fputs(color, tb->out);
			for (i = len; i < width; i++)
				fputs(cellpadding_symbol(tb), tb->out);
			fputs(data, tb->out);
			if (color)
				fputs(UL_COLOR_RESET, tb->out);
			len = width;

		} else if (color) {
			char *p = data;
			size_t art = buffer_get_safe_art_size(buf);

			/* we don't want to colorize tree ascii art */
			if (scols_column_is_tree(cl) && art && art < bytes) {
				fwrite(p, 1, art, tb->out);
				p += art;
			}

			fputs(color, tb->out);
			fputs(p, tb->out);
			fputs(UL_COLOR_RESET, tb->out);
		} else
			fputs(data, tb->out);
	}

	/* minout -- don't fill */
	if (scols_table_is_minout(tb) && is_next_columns_empty(tb, cl, ln))
		return 0;

	/* default -- fill except last column */
	if (!scols_table_is_maxout(tb) && is_last)
		return 0;

	/* fill rest of cell with space */
	for(i = len; i < width; i++)
		fputs(cellpadding_symbol(tb), tb->out);

	if (len > width && !scols_column_is_trunc(cl)) {
		DBG(COL, ul_debugobj(cl, "*** data len=%zu > column width=%zu", len, width));
		print_newline_padding(tb, cl, ln, buffer_get_size(buf));	/* next column starts on next line */

	} else if (!is_last)
		fputs(colsep(tb), tb->out);		/* columns separator */

	return 0;
}

int __cell_to_buffer(struct libscols_table *tb,
			  struct libscols_line *ln,
			  struct libscols_column *cl,
			  struct libscols_buffer *buf)
{
	const char *data;
	struct libscols_cell *ce;
	int rc = 0;

	assert(tb);
	assert(ln);
	assert(cl);
	assert(buf);
	assert(cl->seqnum <= tb->ncols);

	buffer_reset_data(buf);

	ce = scols_line_get_cell(ln, cl->seqnum);
	data = ce ? scols_cell_get_data(ce) : NULL;

	if (!scols_column_is_tree(cl))
		return data ? buffer_set_data(buf, data) : 0;

	/*
	 * Group stuff
	 */
	if (!scols_table_is_json(tb) && cl->is_groups)
		rc = groups_ascii_art_to_buffer(tb, ln, buf);

	/*
	 * Tree stuff
	 */
	if (!rc && ln->parent && !scols_table_is_json(tb)) {
		rc = tree_ascii_art_to_buffer(tb, ln->parent, buf);

		if (!rc && is_last_child(ln))
			rc = buffer_append_data(buf, right_symbol(tb));
		else if (!rc)
			rc = buffer_append_data(buf, branch_symbol(tb));
	}

	if (!rc && (ln->parent || cl->is_groups) && !scols_table_is_json(tb))
		buffer_set_art_index(buf);

	if (!rc && data)
		rc = buffer_append_data(buf, data);
	return rc;
}

/*
 * Prints data. Data can be printed in more formats (raw, NAME=xxx pairs), and
 * control and non-printable characters can be encoded in the \x?? encoding.
 */
static int print_line(struct libscols_table *tb,
		      struct libscols_line *ln,
		      struct libscols_buffer *buf)
{
	int rc = 0, pending = 0;
	struct libscols_column *cl;
	struct libscols_iter itr;

	assert(ln);

	DBG(LINE, ul_debugobj(ln, "printing line"));

	/* regular line */
	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (rc == 0 && scols_table_next_column(tb, &itr, &cl) == 0) {
		if (scols_column_is_hidden(cl))
			continue;
		rc = __cell_to_buffer(tb, ln, cl, buf);
		if (rc == 0)
			rc = print_data(tb, cl, ln,
					scols_line_get_cell(ln, cl->seqnum),
					buf);
		if (rc == 0 && cl->pending_data)
			pending = 1;
	}

	/* extra lines of the multi-line cells */
	while (rc == 0 && pending) {
		DBG(LINE, ul_debugobj(ln, "printing pending data"));
		pending = 0;
		fputs(linesep(tb), tb->out);
		tb->termlines_used++;
		scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
		while (rc == 0 && scols_table_next_column(tb, &itr, &cl) == 0) {
			if (scols_column_is_hidden(cl))
				continue;
			if (cl->pending_data) {
				rc = print_pending_data(tb, cl, ln, scols_line_get_cell(ln, cl->seqnum));
				if (rc == 0 && cl->pending_data)
					pending = 1;
			} else
				print_empty_cell(tb, cl, ln, buffer_get_size(buf));
		}
	}

	return 0;
}

int __scols_print_title(struct libscols_table *tb)
{
	int rc, color = 0;
	mbs_align_t align;
	size_t width, len = 0, bufsz, titlesz;
	char *title = NULL, *buf = NULL;

	assert(tb);

	if (!tb->title.data)
		return 0;

	DBG(TAB, ul_debugobj(tb, "printing title"));

	/* encode data */
	if (tb->no_encode) {
		len = bufsz = strlen(tb->title.data) + 1;
		buf = strdup(tb->title.data);
		if (!buf) {
			rc = -ENOMEM;
			goto done;
		}
	} else {
		bufsz = mbs_safe_encode_size(strlen(tb->title.data)) + 1;
		if (bufsz == 1) {
			DBG(TAB, ul_debugobj(tb, "title is empty string -- ignore"));
			return 0;
		}
		buf = malloc(bufsz);
		if (!buf) {
			rc = -ENOMEM;
			goto done;
		}

		if (!mbs_safe_encode_to_buffer(tb->title.data, &len, buf, NULL) ||
		    !len || len == (size_t) -1) {
			rc = -EINVAL;
			goto done;
		}
	}

	/* truncate and align */
	width = tb->is_term ? tb->termwidth : 80;
	titlesz = width + bufsz;

	title = malloc(titlesz);
	if (!title) {
		rc = -EINVAL;
		goto done;
	}

	switch (scols_cell_get_alignment(&tb->title)) {
	case SCOLS_CELL_FL_RIGHT:
		align = MBS_ALIGN_RIGHT;
		break;
	case SCOLS_CELL_FL_CENTER:
		align = MBS_ALIGN_CENTER;
		break;
	case SCOLS_CELL_FL_LEFT:
	default:
		align = MBS_ALIGN_LEFT;
		/*
		 * Don't print extra blank chars after the title if on left
		 * (that's same as we use for the last column in the table).
		 */
		if (len < width
		    && !scols_table_is_maxout(tb)
		    && isblank(*titlepadding_symbol(tb)))
			width = len;
		break;

	}

	/* copy from buf to title and align to width with title_padding */
	rc = mbsalign_with_padding(buf, title, titlesz,
			&width, align,
			0, (int) *titlepadding_symbol(tb));

	if (rc == -1) {
		rc = -EINVAL;
		goto done;
	}

	if (tb->colors_wanted && tb->title.color)
		color = 1;
	if (color)
		fputs(tb->title.color, tb->out);

	fputs(title, tb->out);

	if (color)
		fputs(UL_COLOR_RESET, tb->out);

	fputc('\n', tb->out);
	rc = 0;
done:
	free(buf);
	free(title);
	DBG(TAB, ul_debugobj(tb, "printing title done [rc=%d]", rc));
	return rc;
}

int __scols_print_header(struct libscols_table *tb, struct libscols_buffer *buf)
{
	int rc = 0;
	struct libscols_column *cl;
	struct libscols_iter itr;

	assert(tb);

	if ((tb->header_printed == 1 && tb->header_repeat == 0) ||
	    scols_table_is_noheadings(tb) ||
	    scols_table_is_export(tb) ||
	    scols_table_is_json(tb) ||
	    list_empty(&tb->tb_lines))
		return 0;

	DBG(TAB, ul_debugobj(tb, "printing header"));

	/* set the width according to the size of the data */
	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (rc == 0 && scols_table_next_column(tb, &itr, &cl) == 0) {
		if (scols_column_is_hidden(cl))
			continue;

		buffer_reset_data(buf);

		if (cl->is_groups
		    && scols_table_is_tree(tb) && scols_column_is_tree(cl)) {
			size_t i;
			for (i = 0; i < tb->grpset_size + 1; i++) {
				rc = buffer_append_data(buf, " ");
				if (rc)
					break;
			}
		}
		if (!rc)
			rc = buffer_append_data(buf, scols_cell_get_data(&cl->header));
		if (!rc)
			rc = print_data(tb, cl, NULL, &cl->header, buf);
	}

	if (rc == 0) {
		fputs(linesep(tb), tb->out);
		tb->termlines_used++;
	}

	tb->header_printed = 1;
	tb->header_next = tb->termlines_used + tb->termheight;
	if (tb->header_repeat)
		DBG(TAB, ul_debugobj(tb, "\tnext header: %zu [current=%zu, rc=%d]",
					tb->header_next, tb->termlines_used, rc));
	return rc;
}


int __scols_print_range(struct libscols_table *tb,
			struct libscols_buffer *buf,
			struct libscols_iter *itr,
			struct libscols_line *end)
{
	int rc = 0;
	struct libscols_line *ln;

	assert(tb);
	DBG(TAB, ul_debugobj(tb, "printing range"));

	while (rc == 0 && scols_table_next_line(tb, itr, &ln) == 0) {

		int last = scols_iter_is_last(itr);

		if (scols_table_is_json(tb))
			ul_jsonwrt_object_open(&tb->json, NULL);

		rc = print_line(tb, ln, buf);

		if (scols_table_is_json(tb))
			ul_jsonwrt_object_close(&tb->json, last);
		else if (last == 0 && tb->no_linesep == 0) {
			fputs(linesep(tb), tb->out);
			tb->termlines_used++;
		}

		if (end && ln == end)
			break;

		if (!last && want_repeat_header(tb))
			__scols_print_header(tb, buf);
	}

	return rc;

}

int __scols_print_table(struct libscols_table *tb, struct libscols_buffer *buf)
{
	struct libscols_iter itr;

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	return __scols_print_range(tb, buf, &itr, NULL);
}

/* scols_walk_tree() callback to print tree line */
static int print_tree_line(struct libscols_table *tb,
			   struct libscols_line *ln,
			   struct libscols_column *cl __attribute__((__unused__)),
			   void *data)
{
	struct libscols_buffer *buf = (struct libscols_buffer *) data;
	int rc;

	DBG(LINE, ul_debugobj(ln, "   printing tree line"));

	if (scols_table_is_json(tb))
		ul_jsonwrt_object_open(&tb->json, NULL);

	rc = print_line(tb, ln, buf);
	if (rc)
		return rc;

	if (has_children(ln)) {
		if (scols_table_is_json(tb))
			ul_jsonwrt_array_open(&tb->json, "children");
		else {
			/* between parent and child is separator */
			fputs(linesep(tb), tb->out);
			tb->termlines_used++;
		}
	} else {
		int last;

		/* terminate all open last children for JSON */
		if (scols_table_is_json(tb)) {
			do {
				last = (is_child(ln) && is_last_child(ln)) ||
				       (is_tree_root(ln) && is_last_tree_root(tb, ln));

				ul_jsonwrt_object_close(&tb->json, last);
				if (last && is_child(ln))
					ul_jsonwrt_array_close(&tb->json, last);
				ln = ln->parent;
			} while(ln && last);

		} else if (tb->no_linesep == 0) {
			int last_in_tree = scols_walk_is_last(tb, ln);

			if (last_in_tree == 0) {
				/* standard output */
				fputs(linesep(tb), tb->out);
				tb->termlines_used++;
			}
		}
	}

	return 0;
}

int __scols_print_tree(struct libscols_table *tb, struct libscols_buffer *buf)
{
	assert(tb);
	DBG(TAB, ul_debugobj(tb, "----printing-tree-----"));

	return scols_walk_tree(tb, NULL, print_tree_line, (void *) buf);
}

static size_t strlen_line(struct libscols_line *ln)
{
	size_t i, sz = 0;

	assert(ln);

	for (i = 0; i < ln->ncells; i++) {
		struct libscols_cell *ce = scols_line_get_cell(ln, i);
		const char *data = ce ? scols_cell_get_data(ce) : NULL;

		sz += data ? strlen(data) : 0;
	}

	return sz;
}

void __scols_cleanup_printing(struct libscols_table *tb, struct libscols_buffer *buf)
{
	if (!tb)
		return;

	free_buffer(buf);

	if (tb->priv_symbols) {
		scols_table_set_symbols(tb, NULL);
		tb->priv_symbols = 0;
	}
}

int __scols_initialize_printing(struct libscols_table *tb, struct libscols_buffer **buf)
{
	size_t bufsz, extra_bufsz = 0;
	struct libscols_line *ln;
	struct libscols_iter itr;
	int rc;

	DBG(TAB, ul_debugobj(tb, "initialize printing"));
	*buf = NULL;

	if (!tb->symbols) {
		rc = scols_table_set_default_symbols(tb);
		if (rc)
			goto err;
		tb->priv_symbols = 1;
	} else
		tb->priv_symbols = 0;

	if (tb->format == SCOLS_FMT_HUMAN)
		tb->is_term = tb->termforce == SCOLS_TERMFORCE_NEVER  ? 0 :
			      tb->termforce == SCOLS_TERMFORCE_ALWAYS ? 1 :
			      isatty(STDOUT_FILENO);

	if (tb->is_term) {
		size_t width = (size_t) scols_table_get_termwidth(tb);

		if (tb->termreduce > 0 && tb->termreduce < width) {
			width -= tb->termreduce;
			scols_table_set_termwidth(tb, width);
		}
		bufsz = width;
	} else
		bufsz = BUFSIZ;

	if (!tb->is_term || tb->format != SCOLS_FMT_HUMAN || scols_table_is_tree(tb))
		tb->header_repeat = 0;

	/*
	 * Estimate extra space necessary for tree, JSON or another output
	 * decoration.
	 */
	if (scols_table_is_tree(tb))
		extra_bufsz += tb->nlines * strlen(vertical_symbol(tb));

	switch (tb->format) {
	case SCOLS_FMT_RAW:
		extra_bufsz += tb->ncols;			/* separator between columns */
		break;
	case SCOLS_FMT_JSON:
		ul_jsonwrt_init(&tb->json, tb->out, 0);
		extra_bufsz += tb->nlines * 3;		/* indentation */
		/* fallthrough */
	case SCOLS_FMT_EXPORT:
	{
		struct libscols_column *cl;

		scols_reset_iter(&itr, SCOLS_ITER_FORWARD);

		while (scols_table_next_column(tb, &itr, &cl) == 0) {
			if (scols_column_is_hidden(cl))
				continue;
			extra_bufsz += strlen(scols_cell_get_data(&cl->header));	/* data */
			extra_bufsz += 2;						/* separators */
		}
		break;
	}
	case SCOLS_FMT_HUMAN:
		break;
	}

	/*
	 * Enlarge buffer if necessary, the buffer should be large enough to
	 * store line data and tree ascii art (or another decoration).
	 */
	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_line(tb, &itr, &ln) == 0) {
		size_t sz;

		sz = strlen_line(ln) + extra_bufsz;
		if (sz > bufsz)
			bufsz = sz;
	}

	*buf = new_buffer(bufsz + 1);	/* data + space for \0 */
	if (!*buf) {
		rc = -ENOMEM;
		goto err;
	}

	/*
	 * Make sure groups members are in the same orders as the tree
	 */
	if (has_groups(tb) && scols_table_is_tree(tb))
		scols_groups_fix_members_order(tb);

	if (tb->format == SCOLS_FMT_HUMAN) {
		rc = __scols_calculate(tb, *buf);
		if (rc != 0)
			goto err;
	}

	return 0;
err:
	__scols_cleanup_printing(tb, *buf);
	return rc;
}

