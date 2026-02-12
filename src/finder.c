// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "finder.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buffer.h"
#include "editor.h"
#include "utf8.h"
#include "util.h"

#define KEY_BACKSPACE 127

typedef struct {
  char *pattern;
  char *base;
  bool negated;
  bool dir_only;
  bool has_slash;
  bool anchored;
} IgnoreRule;

typedef struct {
  IgnoreRule *rules;
  size_t len;
  size_t cap;
} IgnoreList;

typedef struct {
  size_t index;
  int score;
} FinderMatch;

static void ignore_list_init(IgnoreList *list) {
  list->rules = NULL;
  list->len = 0;
  list->cap = 0;
}

static void ignore_rule_free(IgnoreRule *rule) {
  free(rule->pattern);
  free(rule->base);
  rule->pattern = NULL;
  rule->base = NULL;
}

static void ignore_list_free(IgnoreList *list) {
  for (size_t i = 0; i < list->len; i++) {
    ignore_rule_free(&list->rules[i]);
  }
  free(list->rules);
  list->rules = NULL;
  list->len = 0;
  list->cap = 0;
}

static void ignore_list_truncate(IgnoreList *list, size_t len) {
  while (list->len > len) {
    list->len--;
    ignore_rule_free(&list->rules[list->len]);
  }
}

static void ignore_list_add(IgnoreList *list, IgnoreRule *rule) {
  if (list->len + 1 > list->cap) {
    size_t next = list->cap ? list->cap * 2 : 16;
    IgnoreRule *rules = realloc(list->rules, next * sizeof(*rules));
    if (!rules) {
      die("realloc");
    }
    list->rules = rules;
    list->cap = next;
  }
  list->rules[list->len++] = *rule;
}

static void finder_files_clear(Editor *ed) {
  for (size_t i = 0; i < ed->finder.file_count; i++) {
    free(ed->finder.files[i]);
  }
  free(ed->finder.files);
  ed->finder.files = NULL;
  ed->finder.file_count = 0;
  ed->finder.file_cap = 0;
}

static void finder_matches_clear(Editor *ed) {
  ed->finder.match_count = 0;
  ed->finder.selection = 0;
  ed->finder.scroll = 0;
}

static char *path_join(const char *left, const char *right) {
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  bool need_sep = left_len > 0 && left[left_len - 1] != '/';
  size_t total = left_len + (need_sep ? 1 : 0) + right_len + 1;
  char *out = malloc(total);
  if (!out) {
    die("malloc");
  }
  memcpy(out, left, left_len);
  size_t offset = left_len;
  if (need_sep) {
    out[offset++] = '/';
  }
  memcpy(out + offset, right, right_len);
  out[offset + right_len] = '\0';
  return out;
}

static bool path_is_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

static bool path_is_file(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return false;
  }
  return S_ISREG(st.st_mode);
}

static char *path_dirname(const char *path) {
  char *copy = strdup(path);
  if (!copy) {
    die("strdup");
  }
  char *slash = strrchr(copy, '/');
  if (!slash) {
    copy[0] = '.';
    copy[1] = '\0';
    return copy;
  }
  if (slash == copy) {
    slash[1] = '\0';
    return copy;
  }
  *slash = '\0';
  return copy;
}

static char *find_project_root(const char *start_dir) {
  char resolved[PATH_MAX];
  if (!realpath(start_dir, resolved)) {
    return NULL;
  }

  char current[PATH_MAX];
  snprintf(current, sizeof(current), "%s", resolved);

  while (true) {
    char *git_path = path_join(current, ".git");
    bool has_git = path_is_dir(git_path);
    free(git_path);
    if (has_git) {
      return strdup(current);
    }
    if (strcmp(current, "/") == 0) {
      break;
    }
    char *slash = strrchr(current, '/');
    if (!slash) {
      break;
    }
    if (slash == current) {
      current[1] = '\0';
    } else {
      *slash = '\0';
    }
  }
  return NULL;
}

static char *project_root_for_editor(Editor *ed) {
  char cwd[PATH_MAX];
  if (ed->buffer.file_path && ed->buffer.file_path[0] == '/') {
    char *dir = path_dirname(ed->buffer.file_path);
    char *root = find_project_root(dir);
    free(dir);
    return root;
  }
  if (!getcwd(cwd, sizeof(cwd))) {
    return NULL;
  }
  return find_project_root(cwd);
}

static void trim_whitespace(char *line) {
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                     line[len - 1] == '\r' || line[len - 1] == '\n')) {
    line[--len] = '\0';
  }
  size_t start = 0;
  while (line[start] == ' ' || line[start] == '\t') {
    start++;
  }
  if (start > 0) {
    memmove(line, line + start, len - start + 1);
  }
}

static bool parse_gitignore_line(const char *line, const char *base,
                                 IgnoreRule *out) {
  char *copy = strdup(line);
  if (!copy) {
    die("strdup");
  }
  trim_whitespace(copy);
  if (copy[0] == '\0' || copy[0] == '#') {
    free(copy);
    return false;
  }

  bool negated = false;
  if (copy[0] == '!') {
    negated = true;
    memmove(copy, copy + 1, strlen(copy));
  }

  bool anchored = false;
  if (copy[0] == '/') {
    anchored = true;
    memmove(copy, copy + 1, strlen(copy));
  }

  size_t len = strlen(copy);
  bool dir_only = false;
  if (len > 0 && copy[len - 1] == '/') {
    dir_only = true;
    copy[len - 1] = '\0';
  }
  if (copy[0] == '\0') {
    free(copy);
    return false;
  }

  out->pattern = copy;
  out->base = strdup(base);
  if (!out->base) {
    die("strdup");
  }
  out->negated = negated;
  out->dir_only = dir_only;
  out->has_slash = strchr(copy, '/') != NULL;
  out->anchored = anchored;
  return true;
}

static size_t load_gitignore(const char *path, const char *base,
                             IgnoreList *list) {
  if (!path_is_file(path)) {
    return 0;
  }
  FILE *file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  size_t added = 0;
  char *line = NULL;
  size_t cap = 0;
  ssize_t len = 0;
  while ((len = getline(&line, &cap, file)) != -1) {
    if (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[len - 1] = '\0';
    }
    IgnoreRule rule = {0};
    if (parse_gitignore_line(line, base, &rule)) {
      ignore_list_add(list, &rule);
      added++;
    }
  }
  free(line);
  fclose(file);
  return added;
}

static const char *path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    return path;
  }
  return slash + 1;
}

static bool path_has_prefix(const char *path, const char *prefix) {
  size_t prefix_len = strlen(prefix);
  if (prefix_len == 0) {
    return true;
  }
  if (strncmp(path, prefix, prefix_len) != 0) {
    return false;
  }
  if (path[prefix_len] == '\0') {
    return true;
  }
  return path[prefix_len] == '/';
}

static bool rule_matches(const IgnoreRule *rule, const char *path_rel,
                         bool is_dir) {
  if (rule->dir_only && !is_dir) {
    return false;
  }
  if (rule->has_slash || rule->anchored) {
    if (!path_has_prefix(path_rel, rule->base)) {
      return false;
    }
    const char *rel = path_rel + strlen(rule->base);
    if (rule->base[0] != '\0' && rel[0] == '/') {
      rel++;
    }
    if (rel[0] == '\0') {
      return false;
    }
    return fnmatch(rule->pattern, rel, FNM_PATHNAME) == 0;
  }
  const char *name = path_basename(path_rel);
  return fnmatch(rule->pattern, name, 0) == 0;
}

static bool is_ignored(const IgnoreList *list, const char *path_rel,
                       bool is_dir) {
  bool ignored = false;
  for (size_t i = 0; i < list->len; i++) {
    if (rule_matches(&list->rules[i], path_rel, is_dir)) {
      ignored = !list->rules[i].negated;
    }
  }
  return ignored;
}

static void finder_files_add(Editor *ed, const char *path) {
  if (ed->finder.file_count + 1 > ed->finder.file_cap) {
    size_t next = ed->finder.file_cap ? ed->finder.file_cap * 2 : 256;
    char **files = realloc(ed->finder.files, next * sizeof(*files));
    if (!files) {
      die("realloc");
    }
    ed->finder.files = files;
    ed->finder.file_cap = next;
  }
  ed->finder.files[ed->finder.file_count] = strdup(path);
  if (!ed->finder.files[ed->finder.file_count]) {
    die("strdup");
  }
  ed->finder.file_count++;
}

static void scan_project_dir(Editor *ed, const char *root,
                             const char *dir_rel, IgnoreList *list) {
  char *dir_path = dir_rel[0] == '\0' ? strdup(root) : path_join(root, dir_rel);
  if (!dir_path) {
    die("strdup");
  }

  size_t rule_start = list->len;
  char *gitignore_path = path_join(dir_path, ".gitignore");
  load_gitignore(gitignore_path, dir_rel, list);
  free(gitignore_path);

  DIR *dir = opendir(dir_path);
  if (!dir) {
    ignore_list_truncate(list, rule_start);
    free(dir_path);
    return;
  }

  struct dirent *entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (strcmp(name, ".git") == 0) {
      continue;
    }
    char *rel = NULL;
    if (dir_rel[0] == '\0') {
      rel = strdup(name);
    } else {
      char *tmp = path_join(dir_rel, name);
      rel = tmp;
    }
    if (!rel) {
      die("strdup");
    }
    char *full_path = path_join(root, rel);
    if (!full_path) {
      free(rel);
      die("strdup");
    }

    struct stat st;
    bool ok = stat(full_path, &st) == 0;
    bool is_dir = ok && S_ISDIR(st.st_mode);
    bool is_file = ok && S_ISREG(st.st_mode);
    if (ok && !is_ignored(list, rel, is_dir)) {
      if (is_dir) {
        scan_project_dir(ed, root, rel, list);
      } else if (is_file) {
        finder_files_add(ed, rel);
      }
    }

    free(full_path);
    free(rel);
  }

  closedir(dir);
  ignore_list_truncate(list, rule_start);
  free(dir_path);
}

static int fuzzy_score(const char *pattern, const char *text) {
  if (!pattern || pattern[0] == '\0') {
    return 0;
  }
  int score = 0;
  size_t p = 0;
  size_t t = 0;
  int streak = 0;
  size_t pattern_len = strlen(pattern);
  size_t text_len = strlen(text);

  while (text[t] && pattern[p]) {
    char pc = pattern[p];
    char tc = text[t];
    if (tolower((unsigned char)pc) == tolower((unsigned char)tc)) {
      int bonus = 10;
      if (streak > 0) {
        bonus += 6;
      }
      if (t == 0 || text[t - 1] == '/' || text[t - 1] == '_' ||
          text[t - 1] == '-' || text[t - 1] == ' ') {
        bonus += 4;
      }
      score += bonus;
      streak++;
      p++;
    } else {
      streak = 0;
    }
    t++;
  }
  if (pattern[p] != '\0') {
    return -1;
  }
  score -= (int)(text_len - pattern_len);
  return score;
}

static int match_compare(const void *a, const void *b) {
  const FinderMatch *ma = a;
  const FinderMatch *mb = b;
  if (ma->score != mb->score) {
    return mb->score - ma->score;
  }
  if (ma->index < mb->index) {
    return -1;
  }
  if (ma->index > mb->index) {
    return 1;
  }
  return 0;
}

static void finder_update_matches(Editor *ed) {
  finder_matches_clear(ed);
  if (ed->finder.file_count == 0) {
    return;
  }
  FinderMatch *scored = (FinderMatch *)ed->finder.scored;
  if (ed->finder.scored_cap < ed->finder.file_count) {
    size_t next = ed->finder.file_count;
    FinderMatch *matches = realloc(ed->finder.scored,
                                   next * sizeof(*matches));
    if (!matches) {
      die("realloc");
    }
    ed->finder.scored = matches;
    ed->finder.scored_cap = next;
    scored = matches;
  }

  size_t match_count = 0;
  for (size_t i = 0; i < ed->finder.file_count; i++) {
    int score = fuzzy_score(ed->finder.query, ed->finder.files[i]);
    if (score >= 0) {
      scored[match_count].index = i;
      scored[match_count].score = score;
      match_count++;
    }
  }

  if (match_count > 1) {
    qsort(scored, match_count, sizeof(*scored), match_compare);
  }

  if (ed->finder.match_cap < match_count) {
    size_t next = match_count ? match_count : 1;
    size_t *matches = realloc(ed->finder.matches,
                              next * sizeof(*matches));
    if (!matches) {
      die("realloc");
    }
    ed->finder.matches = matches;
    ed->finder.match_cap = next;
  }

  for (size_t i = 0; i < match_count; i++) {
    ed->finder.matches[i] = scored[i].index;
  }
  ed->finder.match_count = match_count;
  ed->finder.selection = 0;
  ed->finder.scroll = 0;
}

size_t finder_list_rows(const Editor *ed) {
  int rows = ed->screen_rows - 4;
  if (rows < 3) {
    rows = 3;
  }
  if (rows > FINDER_MAX_ROWS) {
    rows = FINDER_MAX_ROWS;
  }
  return (size_t)rows;
}

size_t finder_modal_width(const Editor *ed) {
  int width = ed->screen_cols - 6;
  if (width > 70) {
    width = 70;
  }
  if (width < 30) {
    width = ed->screen_cols - 2;
  }
  if (width < 10) {
    width = 10;
  }
  return (size_t)width;
}

static void finder_adjust_scroll(Editor *ed) {
  if (ed->finder.match_count == 0) {
    ed->finder.scroll = 0;
    return;
  }
  size_t rows = finder_list_rows(ed);
  if (ed->finder.selection < ed->finder.scroll) {
    ed->finder.scroll = ed->finder.selection;
  } else if (ed->finder.selection >= ed->finder.scroll + rows) {
    ed->finder.scroll = ed->finder.selection - rows + 1;
  }
}

void finder_move_selection(Editor *ed, int delta) {
  if (!ed->finder.active || ed->finder.match_count == 0) {
    return;
  }
  size_t sel = ed->finder.selection;
  if (delta < 0) {
    size_t step = (size_t)(-delta);
    if (sel < step) {
      sel = 0;
    } else {
      sel -= step;
    }
  } else if (delta > 0) {
    size_t step = (size_t)delta;
    size_t max = ed->finder.match_count - 1;
    if (sel + step > max) {
      sel = max;
    } else {
      sel += step;
    }
  }
  ed->finder.selection = sel;
  finder_adjust_scroll(ed);
}

static void finder_open_file(Editor *ed) {
  if (ed->finder.match_count == 0) {
    return;
  }
  size_t index = ed->finder.matches[ed->finder.selection];
  if (index >= ed->finder.file_count) {
    return;
  }
  char *full_path = path_join(ed->finder.project_root,
                              ed->finder.files[index]);
  if (!full_path) {
    return;
  }
  if (access(full_path, R_OK) != 0) {
    snprintf(ed->status_msg, sizeof(ed->status_msg),
             "Open failed: %s", strerror(errno));
    free(full_path);
    return;
  }
  buffer_load_file(&ed->buffer, full_path);
  buffer_init_syntax(&ed->buffer, ed->buffer.file_path);
  ed->dirty = false;
  ed->frame.row = 0;
  ed->frame.col = 0;
  ed->row_offset = 0;
  ed->col_offset = 0;
  snprintf(ed->status_msg, sizeof(ed->status_msg), "Opened %s",
           ed->finder.files[index]);
  free(full_path);
}

static void finder_load_project(Editor *ed) {
  finder_files_clear(ed);
  IgnoreList list;
  ignore_list_init(&list);
  scan_project_dir(ed, ed->finder.project_root, "", &list);
  ignore_list_free(&list);
}

bool finder_open(Editor *ed) {
  if (ed->prompt_active) {
    snprintf(ed->status_msg, sizeof(ed->status_msg),
             "Finish prompt first");
    return false;
  }
  char *root = project_root_for_editor(ed);
  if (!root) {
    snprintf(ed->status_msg, sizeof(ed->status_msg), "No project root");
    return false;
  }
  if (!ed->finder.project_root ||
      strcmp(ed->finder.project_root, root) != 0) {
    free(ed->finder.project_root);
    ed->finder.project_root = root;
    finder_load_project(ed);
  } else {
    free(root);
    if (ed->finder.file_count == 0) {
      finder_load_project(ed);
    }
  }

  ed->finder.active = true;
  ed->finder.query_len = 0;
  ed->finder.query[0] = '\0';
  finder_update_matches(ed);
  return true;
}

void finder_close(Editor *ed) {
  ed->finder.active = false;
}

void finder_reset(Editor *ed) {
  finder_files_clear(ed);
  ed->finder.active = false;
  ed->finder.query_len = 0;
  ed->finder.query[0] = '\0';
  free(ed->finder.project_root);
  ed->finder.project_root = NULL;
  free(ed->finder.matches);
  ed->finder.matches = NULL;
  ed->finder.match_count = 0;
  ed->finder.match_cap = 0;
  free(ed->finder.scored);
  ed->finder.scored = NULL;
  ed->finder.scored_cap = 0;
}

void finder_handle_key(Editor *ed, uint8_t key) {
  if (!ed->finder.active) {
    return;
  }
  if (key == CTRL_KEY('g')) {
    finder_close(ed);
    return;
  }
  if (key == '\r') {
    finder_open_file(ed);
    finder_close(ed);
    return;
  }
  if (key == KEY_BACKSPACE) {
    if (ed->finder.query_len > 0) {
      size_t new_len = utf8_prev_boundary(ed->finder.query,
                                          ed->finder.query_len);
      ed->finder.query_len = new_len;
      ed->finder.query[ed->finder.query_len] = '\0';
      finder_update_matches(ed);
    }
    return;
  }
  if (key == CTRL_KEY('p')) {
    finder_move_selection(ed, -1);
    return;
  }
  if (key == CTRL_KEY('n')) {
    finder_move_selection(ed, 1);
    return;
  }
  if (key >= 32 && key != KEY_BACKSPACE &&
      ed->finder.query_len + 1 < sizeof(ed->finder.query)) {
    ed->finder.query[ed->finder.query_len++] = (char)key;
    ed->finder.query[ed->finder.query_len] = '\0';
    finder_update_matches(ed);
  }
}
