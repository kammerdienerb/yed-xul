#include <yed/plugin.h>

/* COMMANDS */
void xul_take_key(int n_args, char **args);
void xul_bind(int n_args, char **args);
void xul_unbind(int n_args, char **args);
void xul_exit_insert(int n_args, char **args);
/* END COMMANDS */

enum {
    MODE_NORMAL,
    MODE_INSERT,
    N_MODES,
};

static char *mode_strs[] = {
    "NORMAL",
    "INSERT",
};

static char *mode_strs_lowercase[] = {
    "normal",
    "insert",
};

static int mode_completion(char *string, yed_completion_results *results) {
    int status;

    FN_BODY_FOR_COMPLETE_FROM_ARRAY(string, 2, mode_strs_lowercase, results, status);

    return status;
}

typedef struct {
    int    len;
    int    keys[MAX_SEQ_LEN];
    char  *cmd;
    int    key;
    int    n_args;
    char **args;
} key_binding;

static yed_plugin *Self;
static int         mode;
static array_t     mode_bindings[N_MODES];
static int         till_pending; /* 0 = not pending, 1 = pending forward, 2 = pending backward, 3 = pending backward; stop before */
static int         last_till_key;
static char        last_till_op;
static int         num_undo_records_before_insert;
static int         restore_cursor_line;
static int         visual;
static int         last_nav_key;
static int         save_nav_key;
static int         save_action;
static array_t     insert_repeat_keys;
static int         repeating;

void unload(yed_plugin *self);
void edraw(yed_event *event);
void efocus(yed_event *event);
void normal(int key);
void insert(int key);
void bind_keys(void);
void change_mode(int new_mode);
void enter_insert(void);
void exit_insert(void);
void make_binding(int b_mode, int n_keys, int *keys, char *cmd, int n_args, char **args);
void remove_binding(int b_mode, int n_keys, int *keys);

int yed_plugin_boot(yed_plugin *self) {
    int               i;
    yed_event_handler handler;

    YED_PLUG_VERSION_CHECK();

    Self = self;

    for (i = 0; i < N_MODES; i += 1) {
        mode_bindings[i] = array_make(key_binding);
    }

    insert_repeat_keys = array_make(int);

    yed_plugin_set_unload_fn(Self, unload);

    handler.kind = EVENT_PRE_DRAW_EVERYTHING;
    handler.fn   = edraw;
    yed_plugin_add_event_handler(self, handler);

    handler.kind = EVENT_BUFFER_PRE_FOCUS;
    handler.fn   = efocus;
    yed_plugin_add_event_handler(self, handler);

    yed_plugin_set_command(Self, "xul-take-key",    xul_take_key);
    yed_plugin_set_command(Self, "xul-bind",        xul_bind);
    yed_plugin_set_command(Self, "xul-unbind",      xul_unbind);
    yed_plugin_set_command(Self, "xul-exit-insert", xul_exit_insert);

    yed_plugin_set_completion(Self, "xul-mode", mode_completion);
    yed_plugin_set_completion(Self, "xul-bind-compl-arg-0", mode_completion);
    yed_plugin_set_completion(Self, "xul-bind-compl-arg-2", yed_get_completion("command"));
    yed_plugin_set_completion(Self, "xul-unbind-compl-arg-0", mode_completion);

    bind_keys();

    if (yed_get_var("xul-normal-attrs") == NULL) {
        yed_set_var("xul-normal-attrs", "bg !4");
    }
    if (yed_get_var("xul-insert-attrs") == NULL) {
        yed_set_var("xul-insert-attrs", "bg !2");
    }
    if (yed_get_var("xul-insert-no-cursor-line") == NULL) {
        yed_set_var("xul-insert-no-cursor-line", "yes");
    }

    change_mode(MODE_NORMAL);
    yed_set_var("xul-mode", mode_strs[mode]);
    yed_set_var("enable-search-cursor-move", "yes");

    return 0;
}

void unload(yed_plugin *self) {
    int          i, j;
    key_binding *b;

    for (i = 0; i < N_MODES; i += 1) {
        array_traverse(mode_bindings[i], b) {
            if (b->args) {
                for (j = 0; j < b->n_args; j += 1) {
                    free(b->args[j]);
                }
                free(b->args);
            }
            free(b->cmd);
        }
        array_free(mode_bindings[i]);
    }
}

void edraw(yed_event *event) {
    if (mode                     != MODE_NORMAL) { return; }
    if (ys->active_frame         == NULL)        { return; }
    if (ys->active_frame->buffer == NULL)        { return; }
    if (ys->active_frame->buffer->has_selection) { return; }

    visual = 0;
    YEXE("select-lines");
}

void efocus(yed_event *event) {
    if (mode                     != MODE_NORMAL)  { return; }
    if (ys->active_frame         == NULL)         { return; }
    if (ys->active_frame->buffer == NULL)         { return; }


    visual = 0;

    if (ys->active_frame->buffer->has_selection) {
        YEXE("select-off");
    }
}

void bind_keys(void) {
    int   meta_keys[2];
    int   meta_key;
    char *ctrl_h_is_bs;
    char *ctrl_h_is_bs_cpy;
    int   key;
    char  key_str[32];

    meta_keys[0] = ESC;

    meta_keys[1] = ARROW_UP;
    meta_key     = yed_get_key_sequence(2, meta_keys);
    yed_unbind_key(meta_key);
    yed_delete_key_sequence(meta_key);
    meta_keys[1] = ARROW_DOWN;
    meta_key     = yed_get_key_sequence(2, meta_keys);
    yed_unbind_key(meta_key);
    yed_delete_key_sequence(meta_key);
    meta_keys[1] = ARROW_RIGHT;
    meta_key     = yed_get_key_sequence(2, meta_keys);
    yed_unbind_key(meta_key);
    yed_delete_key_sequence(meta_key);
    meta_keys[1] = ARROW_LEFT;
    meta_key     = yed_get_key_sequence(2, meta_keys);
    yed_unbind_key(meta_key);
    yed_delete_key_sequence(meta_key);

    if ((ctrl_h_is_bs = yed_get_var("ctrl-h-is-backspace"))) {
        ctrl_h_is_bs_cpy = strdup(ctrl_h_is_bs);
        yed_unset_var("ctrl-h-is-backspace");
    }

    for (key = 1; key < REAL_KEY_MAX; key += 1) {
        sprintf(key_str, "%d", key);
        YPBIND(Self, key, "xul-take-key", key_str);
    }

    if (ctrl_h_is_bs) {
        yed_set_var("ctrl-h-is-backspace", ctrl_h_is_bs_cpy);
        free(ctrl_h_is_bs_cpy);
    }
}

void change_mode(int new_mode) {
    char         key_str[32];
    key_binding *b;

    array_traverse(mode_bindings[mode], b) {
        yed_unbind_key(b->key);
        if (b->len > 1) {
            yed_delete_key_sequence(b->key);
        } else if (b->key < REAL_KEY_MAX) {
            sprintf(key_str, "%d", b->key);
            YPBIND(Self, b->key, "xul-take-key", key_str);
        }
    }

    array_traverse(mode_bindings[new_mode], b) {
        if (b->len > 1) {
            b->key = yed_plugin_add_key_sequence(Self, b->len, b->keys);
        } else {
            b->key = b->keys[0];
        }

        yed_plugin_bind_key(Self, b->key, b->cmd, b->n_args, b->args);
    }

    visual = 0;

    switch (mode) {
        case MODE_NORMAL:                      break;
        case MODE_INSERT: exit_insert();       break;
    }

    mode = new_mode;

    switch (new_mode) {
        case MODE_NORMAL: {
            break;
        }
        case MODE_INSERT: enter_insert();        break;
    }

    yed_set_var("xul-mode", mode_strs[new_mode]);

    yed_set_var("xul-mode-attrs", yed_get_var("xul-insert-attrs"));

    switch (new_mode) {
        case MODE_NORMAL: yed_set_var("xul-mode-attrs", yed_get_var("xul-normal-attrs")); break;
        case MODE_INSERT: yed_set_var("xul-mode-attrs", yed_get_var("xul-insert-attrs")); break;
    }
}

static void _take_key(int key, char *maybe_key_str) {
    char *key_str, buff[32];

    if (maybe_key_str) {
        key_str = maybe_key_str;
    } else {
        sprintf(buff, "%d", key);
        key_str = buff;
    }

    switch (mode) {
        case MODE_NORMAL: normal(key); break;
        case MODE_INSERT: insert(key); break;
        default:
            LOG_FN_ENTER();
            yed_log("[!] invalid mode (?)");
            LOG_EXIT();
    }
}

void xul_take_key(int n_args, char **args) {
    int key;

    if (n_args != 1) {
        yed_cerr("expected 1 argument, but got %d", n_args);
        return;
    }

    sscanf(args[0], "%d", &key);

    _take_key(key, args[0]);
}

void xul_bind(int n_args, char **args) {
    char *mode_str, *cmd;
    int   b_mode, n_keys, keys[MAX_SEQ_LEN], n_cmd_args;

    if (n_args < 1) {
        yed_cerr("missing 'mode' as first argument");
        return;
    }

    mode_str = args[0];

    if      (strcmp(mode_str, "normal") == 0)    { b_mode = MODE_NORMAL; }
    else if (strcmp(mode_str, "insert") == 0)    { b_mode = MODE_INSERT; }
    else {
        yed_cerr("no mode named '%s'", mode_str);
        return;
    }

    if (n_args < 2) {
        yed_cerr("missing 'keys' as second argument");
        return;
    }

    if (n_args < 3) {
        yed_cerr("missing 'command', 'command_args'... as third and up arguments");
        return;
    }

    n_keys = yed_string_to_keys(args[1], keys);
    if (n_keys == -1) {
        yed_cerr("invalid string of keys '%s'", args[1]);
        return;
    }
    if (n_keys == -2) {
        yed_cerr("too many keys to be a sequence in '%s'", args[1]);
        return;
    }

    cmd = args[2];

    n_cmd_args = n_args - 3;
    make_binding(b_mode, n_keys, keys, cmd, n_cmd_args, args + 3);
}

void xul_unbind(int n_args, char **args) {
    char *mode_str;
    int   b_mode, n_keys, keys[MAX_SEQ_LEN];

    if (n_args < 1) {
        yed_cerr("missing 'mode' as first argument");
        return;
    }

    mode_str = args[0];

    if      (strcmp(mode_str, "normal") == 0)    { b_mode = MODE_NORMAL; }
    else if (strcmp(mode_str, "insert") == 0)    { b_mode = MODE_INSERT; }
    else {
        yed_cerr("no mode named '%s'", mode_str);
        return;
    }

    if (n_args != 2) {
        yed_cerr("missing 'keys' as second argument");
        return;
    }

    n_keys = yed_string_to_keys(args[1], keys);
    if (n_keys == -1) {
        yed_cerr("invalid string of keys '%s'", args[1]);
        return;
    }
    if (n_keys == -2) {
        yed_cerr("too many keys to be a sequence in '%s'", args[1]);
        return;
    }

    remove_binding(b_mode, n_keys, keys);
}

void make_binding(int b_mode, int n_keys, int *keys, char *cmd, int n_args, char **args) {
    int         i;
    key_binding binding, *b;

    if (n_keys <= 0) {
        return;
    }

    binding.len = n_keys;
    for (i = 0; i < n_keys; i += 1) {
        binding.keys[i] = keys[i];
    }
    binding.cmd = strdup(cmd);
    binding.key = KEY_NULL;
    binding.n_args = n_args;
    if (n_args) {
        binding.args = malloc(sizeof(char*) * n_args);
        for (i = 0; i < n_args; i += 1) {
            binding.args[i] = strdup(args[i]);
        }
    } else {
        binding.args = NULL;
    }

    array_push(mode_bindings[b_mode], binding);

    if (b_mode == mode) {
        array_traverse(mode_bindings[mode], b) {
            yed_unbind_key(b->key);
            if (b->len > 1) {
                yed_delete_key_sequence(b->key);
            }
        }

        array_traverse(mode_bindings[mode], b) {
            if (b->len > 1) {
                b->key = yed_plugin_add_key_sequence(Self, b->len, b->keys);
            } else {
                b->key = b->keys[0];
            }

            yed_plugin_bind_key(Self, b->key, b->cmd, b->n_args, b->args);
        }
    }
}

void remove_binding(int b_mode, int n_keys, int *keys) {
    int          i;
    key_binding *b;

    if (n_keys <= 0) {
        return;
    }

    i = 0;
    array_traverse(mode_bindings[b_mode], b) {
        if (b->len == n_keys
        &&  memcmp(b->keys, keys, n_keys * sizeof(int)) == 0) {
            break;
        }
        i += 1;
    }

    if (i == array_len(mode_bindings[b_mode])) { return; }

    if (b_mode == mode) {
        array_traverse(mode_bindings[mode], b) {
            yed_unbind_key(b->key);
            if (b->len > 1) {
                yed_delete_key_sequence(b->key);
            }
        }

        array_delete(mode_bindings[b_mode], i);

        array_traverse(mode_bindings[mode], b) {
            if (b->len > 1) {
                b->key = yed_plugin_add_key_sequence(Self, b->len, b->keys);
            } else {
                b->key = b->keys[0];
            }

            yed_plugin_bind_key(Self, b->key, b->cmd, b->n_args, b->args);
        }
    } else {
        array_delete(mode_bindings[b_mode], i);
    }
}

void xul_exit_insert(int n_args, char **args) {
    change_mode(MODE_NORMAL);
}

static void do_till_fw(int key) {
    yed_frame *f;
    yed_line  *line;
    int        col;
    yed_glyph *g;

    if (!ys->active_frame || !ys->active_frame->buffer)    { goto out; }

    f = ys->active_frame;

    line = yed_buff_get_line(f->buffer, f->cursor_line);

    if (!line)    { goto out; }

    for (col = f->cursor_col + 1; col <= line->visual_width; ) {
        g = yed_line_col_to_glyph(line, col);
        if (g->c == key) {
            yed_set_cursor_within_frame(f, f->cursor_line, col);
            break;
        }
        col += yed_get_glyph_width(*g);
    }

    last_till_key = key;

out:
    till_pending = 0;
    return;
}

static void do_till_bw(int key, int stop_before) {
    yed_frame *f;
    yed_line  *line;
    int        col;
    yed_glyph *g;

    if (!ys->active_frame || !ys->active_frame->buffer)    { goto out; }

    f = ys->active_frame;

    line = yed_buff_get_line(f->buffer, f->cursor_line);

    if (!line)    { goto out; }

    for (col = f->cursor_col - 1; col >= 1;) {
        g = yed_line_col_to_glyph(line, col);
        if (g == NULL) { break; }
        if (g->c == key) {
            yed_set_cursor_within_frame(f,
                                        f->cursor_line,
                                        col + (stop_before * yed_get_glyph_width(*g)));
            break;
        }
        if (col == 1) { break; } /* Didn't find it. Prevent endless loop. */
        col = yed_line_idx_to_col(line, yed_line_col_to_idx(line, col - 1));
    }

    last_till_key = key;

out:
    till_pending = 0;
    return;
}

int nav_common(int key) {
    int has_sel;
    int is_line_sel;
    int save_cursor_line;

    if (till_pending == 1) {
        do_till_fw(key);
        goto out;
    } else if (till_pending > 1) {
        do_till_bw(key, till_pending == 3);
        goto out;
    }

    has_sel     =     ys->active_frame
                   && ys->active_frame->buffer
                   && ys->active_frame->buffer->has_selection;

    is_line_sel =     has_sel
                   && ys->active_frame->buffer->selection.kind == RANGE_LINE;

    if (!has_sel) {
        YEXE("select-lines");
    }

    switch (key) {
        case 'h':
        case ARROW_LEFT:
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            YEXE("cursor-left");
            break;
        case 'H':
            if (!visual && is_line_sel) {
                YEXE("select-off");
                YEXE("select");
            }
            YEXE("cursor-left");
            break;

        case 'j':
        case ARROW_DOWN:
            if (visual) {
                YEXE("cursor-down");
            } else {
                YEXE("select-off");
                YEXE("cursor-down");
                YEXE("select-lines");
            }
            break;
        case 'J':
            YEXE("cursor-down");
            break;

        case 'k':
        case ARROW_UP:
            if (visual) {
                YEXE("cursor-up");
            } else {
                YEXE("select-off");
                YEXE("cursor-up");
                YEXE("select-lines");
            }
            break;
        case 'K':
            YEXE("cursor-up");
            break;

        case 'l':
        case ARROW_RIGHT:
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
        case 'L':
            if (!visual && is_line_sel) {
                YEXE("select-off");
                YEXE("select");
            }
            YEXE("cursor-right");
            break;

        case PAGE_UP:
            if (visual) {
                YEXE("cursor-page-up");
            } else {
                YEXE("select-off");
                YEXE("cursor-page-up");
                YEXE("select-lines");
            }
            break;

        case PAGE_DOWN:
            if (visual) {
                YEXE("cursor-page-up");
            } else {
                YEXE("select-off");
                YEXE("cursor-page-down");
                YEXE("select-lines");
            }
            break;

        case 'w':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
        case 'W':
            if (!visual && is_line_sel) {
                YEXE("select-off");
                YEXE("select");
            }
            YEXE("cursor-next-word");
            break;

        case 'b':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
        case 'B':
            if (!visual && is_line_sel) {
                YEXE("select-off");
                YEXE("select");
            }
            YEXE("cursor-prev-word");
            break;

        case '0':
        case HOME_KEY:
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            YEXE("cursor-line-begin");
            break;

        case '$':
        case END_KEY:
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            YEXE("cursor-line-end");
            break;

        case '{':
            if (!visual) {
                YEXE("select-off");
                YEXE("select-lines");
            }
            save_cursor_line = ys->active_frame ? ys->active_frame->cursor_line : 0;
            YEXE("cursor-up");
            YEXE("cursor-prev-paragraph");
            YEXE("cursor-next-paragraph");
            YEXE("cursor-up");
            if ((ys->active_frame ? ys->active_frame->cursor_line : 0) == save_cursor_line) {
                YEXE("cursor-up");
            }
            break;

        case '}':
            if (!visual) {
                YEXE("select-off");
                YEXE("select-lines");
            }
            save_cursor_line = ys->active_frame ? ys->active_frame->cursor_line : 0;
            YEXE("cursor-next-paragraph");
            YEXE("cursor-up");
            if ((ys->active_frame ? ys->active_frame->cursor_line : 0) == save_cursor_line) {
                YEXE("cursor-down");
                YEXE("cursor-next-paragraph");
                YEXE("cursor-up");
            }
            break;

        case 'g':
            YEXE("cursor-buffer-begin");
            if (!visual) {
                YEXE("select-off");
                YEXE("select-lines");
            }
            break;

        case 'G':
            YEXE("cursor-buffer-end");
            if (!visual) {
                YEXE("select-off");
                YEXE("select-lines");
            }
            break;

        case '/':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            save_cursor_line = ys->active_frame ? ys->active_frame->cursor_line : 0;
            YEXE("find-in-buffer");
            if ((ys->active_frame ? ys->active_frame->cursor_line : 0) != save_cursor_line) {
                YEXE("select-off");
            }
            break;

        case '?':
            YEXE("replace-current-search");
            break;

        case 'n':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            save_cursor_line = ys->active_frame ? ys->active_frame->cursor_line : 0;
            YEXE("find-next-in-buffer");
            if ((ys->active_frame ? ys->active_frame->cursor_line : 0) != save_cursor_line) {
                YEXE("select-off");
                YEXE("select-lines");
            }
            break;

        case 'N':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            save_cursor_line = ys->active_frame ? ys->active_frame->cursor_line : 0;
            YEXE("find-prev-in-buffer");
            if ((ys->active_frame ? ys->active_frame->cursor_line : 0) != save_cursor_line) {
                YEXE("select-off");
                YEXE("select-lines");
            }
            break;

        case 'f':
        case 't':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            till_pending = 1;
            last_till_op = key;
            break;

        case 'F':
        case 'T':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            till_pending = 2 + (key == 'T');
            last_till_op = key;
            break;

        case ';':
            if (!visual) {
                YEXE("select-off");
                YEXE("select");
            }
            break;

        default:
            return 0;
    }

    if (isprint(key)) {
        switch (key) {
            case 'f':
            case 'F':
            case 't':
            case 'T':
                last_nav_key = key;
                break;
            default:
                last_nav_key = tolower(key);
                break;
        }
    } else {
        last_nav_key = key;
    }

out:;
    return 1;
}

void normal(int key) {
    int *key_it;

    if (nav_common(key)) {
        return;
    }

    switch (key) {
        case 'c':
            visual = 0;
            YEXE("yank-selection", "1");
            YEXE("delete-back");
            YEXE("select-off");
            change_mode(MODE_INSERT);
            break;
        case 'd':
            visual = 0;
            YEXE("yank-selection", "1");
            YEXE("delete-back");
            YEXE("select-off");
            YEXE("select-lines");
            break;

        case 'y':
            visual = 0;
            YEXE("yank-selection");
            YEXE("select-off");
            YEXE("select-lines");
            break;

        case 'v':
            visual = !visual;
            YEXE("select-off");
            YEXE("select");
            break;

        case 'V':
            visual = !visual;
            YEXE("select-off");
            YEXE("select-lines");
            break;

        case CTRL_V:
            visual = !visual;
            YEXE("select-off");
            YEXE("select-rect");
            break;

        case 'p':
            visual = 0;
            YEXE("paste-yank-buffer");
            YEXE("select-off");
            YEXE("select-lines");
            break;

        case 'a':
            YEXE("cursor-right");
            goto enter_insert;
        case 'A':
            YEXE("cursor-line-end");
            goto enter_insert;
        case 'i':
enter_insert:
            visual = 0;
            YEXE("select-off");
            change_mode(MODE_INSERT);
            break;

        case DEL_KEY:
            YEXE("select-off");
            YEXE("delete-forward");
            break;

        case 'u':
            visual = 0;
            YEXE("undo");
            YEXE("select-off");
            YEXE("select-lines");
            break;

        case CTRL_R:
            visual = 0;
            YEXE("redo");
            YEXE("select-off");
            YEXE("select-lines");
            break;

        case '.':
            repeating = 1;
            if (save_action == 'a'
            ||  save_action == 'A'
            ||  save_action == 'i') {

                change_mode(MODE_INSERT);
                array_traverse(insert_repeat_keys, key_it) {
                    insert(*key_it);
                }
                change_mode(MODE_NORMAL);
            } else {
                nav_common(save_nav_key);

                if (save_nav_key == 'f' || save_nav_key == 'F'
                ||  save_nav_key == 't' || save_nav_key == 'T') {

                    nav_common(last_till_key);
                }

                normal(save_action);
            }
            repeating = 0;
            break;

        case ':':
            YEXE("command-prompt");
            break;

        case ESC:
        case CTRL_C:
            visual = 0;
            YEXE("select-off");
            YEXE("select-lines");
            break;

        default:
            yed_cerr("[NORMAL] unhandled key %d", key);
    }

    if (key != '.' && isprint(key)) {
        save_nav_key = last_nav_key;
        save_action  = key;
    }
}

void insert(int key) {
    char key_str[32];

    switch (key) {
        case ARROW_LEFT:
            YEXE("cursor-left");
            break;

        case ARROW_DOWN:
            YEXE("cursor-down");
            break;

        case ARROW_UP:
            YEXE("cursor-up");
            break;

        case ARROW_RIGHT:
            YEXE("cursor-right");
            break;

        case PAGE_UP:
            YEXE("cursor-page-up");
            break;

        case PAGE_DOWN:
            YEXE("cursor-page-down");
            break;

        case HOME_KEY:
            YEXE("cursor-line-begin");
            break;

        case END_KEY:
            YEXE("cursor-line-end");
            break;

        case BACKSPACE:
            YEXE("delete-back");
            break;

        case DEL_KEY:
            YEXE("delete-forward");
            break;

        case ESC:
        case CTRL_C:
            change_mode(MODE_NORMAL);
            break;

        default:
            if (key == ENTER || key == TAB || key == MBYTE || !iscntrl(key)) {
                snprintf(key_str, sizeof(key_str), "%d", key);
                YEXE("insert", key_str);
            } else {
                yed_cerr("[INSERT] unhandled key %d", key);
            }
    }

    if (mode == MODE_INSERT && !repeating) {
        array_push(insert_repeat_keys, key);
    }
}

void enter_insert(void) {
    yed_frame  *frame;
    yed_buffer *buff;

    if (!repeating) {
        array_clear(insert_repeat_keys);
    }

    frame = ys->active_frame;

    if (frame) {
        buff = frame->buffer;

        if (buff) {
            num_undo_records_before_insert = yed_get_undo_num_records(buff);
        }
    }

    if (yed_get_var("xul-insert-no-cursor-line")
    &&  yed_get_var("cursor-line")) {
        restore_cursor_line = 1;
        yed_set_var("cursor-line", "no");
    }
}

void exit_insert(void) {
    yed_frame  *frame;
    yed_buffer *buff;

    frame = ys->active_frame;

    if (frame) {
        buff = frame->buffer;

        if (buff) {

            while (yed_get_undo_num_records(buff) > num_undo_records_before_insert + 1) {
                yed_merge_undo_records(buff);
            }
        }
    }

    if (restore_cursor_line
    &&  yed_get_var("xul-insert-no-cursor-line")) {
        yed_set_var("cursor-line", "yes");
        restore_cursor_line = 0;
    }
    YEXE("select-lines");
}
