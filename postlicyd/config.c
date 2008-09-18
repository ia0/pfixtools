/******************************************************************************/
/*          pfixtools: a collection of postfix related tools                  */
/*          ~~~~~~~~~                                                         */
/*  ________________________________________________________________________  */
/*                                                                            */
/*  Redistribution and use in source and binary forms, with or without        */
/*  modification, are permitted provided that the following conditions        */
/*  are met:                                                                  */
/*                                                                            */
/*  1. Redistributions of source code must retain the above copyright         */
/*     notice, this list of conditions and the following disclaimer.          */
/*  2. Redistributions in binary form must reproduce the above copyright      */
/*     notice, this list of conditions and the following disclaimer in the    */
/*     documentation and/or other materials provided with the distribution.   */
/*  3. The names of its contributors may not be used to endorse or promote    */
/*     products derived from this software without specific prior written     */
/*     permission.                                                            */
/*                                                                            */
/*  THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND   */
/*  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE     */
/*  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR        */
/*  PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS    */
/*  BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR    */
/*  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF      */
/*  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS  */
/*  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN   */
/*  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)   */
/*  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF    */
/*  THE POSSIBILITY OF SUCH DAMAGE.                                           */
/******************************************************************************/

/*
 * Copyright © 2008 Florent Bruneau
 */

#include "file.h"
#include "config.h"
#include "str.h"

#define config_param_register(Param)

/* Filter to execute on "CONNECT"
 */
config_param_register("client_filter");

/* Filter to execute on "MAIL FROM"
 */
config_param_register("sender_filter");

/* Filter to execute on "RCPT TO"
 */
config_param_register("recipient_filter");

/* Filter to execute on "DATA"
 */
config_param_register("data_filter");

/* Filter to execute on "END-OF-DATA"
 */
config_param_register("end_of_data_filter");

/* Filter to execute on "ETRN"
 */
config_param_register("etrn_filter");

/* Filter to execute on "HELO"
 */
config_param_register("helo_filter");
config_param_register("ehlo_filter");

/* Filter to execute on "VRFY"
 */
config_param_register("verify_filter");

static inline config_t *config_new(void)
{
    config_t *config = p_new(config_t, 1);
    return config;
}

static void config_close(config_t *config)
{
    for (int i = 0 ; i < SMTP_count ; ++i) {
        config->entry_points[i] = -1;
    }
    array_deep_wipe(config->filters, filter_wipe);
    array_deep_wipe(config->params, filter_params_wipe);
}

void config_delete(config_t **config)
{
    if (*config) {
        config_close(*config);
        p_delete(config);
    }
}


static bool config_second_pass(config_t *config)
{
    bool ok = true;
    if (config->filters.len > 0) {
#       define QSORT_TYPE filter_t
#       define QSORT_BASE config->filters.data
#       define QSORT_NELT config->filters.len
#       define QSORT_LT(a,b) strcmp(a->name, b->name) < 0
#       include "qsort.c"
    }

    foreach (filter_t *filter, config->filters) {
        if (!filter_update_references(filter, &config->filters)) {
            ok = false;
            break;
        }
    }}
    if (!ok) {
        return false;
    }
    if (!filter_check_safety(&config->filters)) {
        return false;
    }

    ok = false;
    foreach (filter_param_t *param, config->params) {
        switch (param->type) {
#define   CASE(Param, State)                                                   \
            case ATK_ ## Param ## _FILTER:                                     \
              ok = true;                                                       \
              config->entry_points[SMTP_ ## State]                             \
                  = filter_find_with_name(&config->filters, param->value);     \
              break;
          CASE(CLIENT,      CONNECT)
          CASE(EHLO,        EHLO)
          CASE(HELO,        HELO)
          CASE(SENDER,      MAIL)
          CASE(RECIPIENT,   RCPT)
          CASE(DATA,        DATA)
          CASE(END_OF_DATA, END_OF_MESSAGE)
          CASE(VERIFY,      VRFY)
          CASE(ETRN,        ETRN)
#undef    CASE
          default: break;
        }
    }}
    array_deep_wipe(config->params, filter_params_wipe);

    if (!ok) {
        syslog(LOG_ERR, "no entry point defined");
    }

    return ok;
}

static bool config_load(config_t *config)
{
    filter_t filter;
    file_map_t map;
    const char *p;
    int line = 0;
    const char *linep;

    char key[BUFSIZ];
    char value[BUFSIZ];
    ssize_t key_len, value_len;

    if (!file_map_open(&map, config->filename, false)) {
        return false;
    }

    config_close(config);
    filter_init(&filter);
    linep = p = map.map;

#define READ_ERROR(Fmt, ...)                                                   \
    do {                                                                       \
        syslog(LOG_ERR, "config file %s:%d:%d: " Fmt, config->filename,        \
               line + 1, p - linep + 1, ##__VA_ARGS__);                        \
        goto error;                                                            \
    } while (0)
#define ADD_IN_BUFFER(Buffer, Len, Char)                                       \
    do {                                                                       \
        if ((Len) >= BUFSIZ - 1) {                                             \
            READ_ERROR("unreasonnable long line");                             \
        }                                                                      \
        (Buffer)[(Len)++] = (Char);                                            \
        (Buffer)[(Len)]   = '\0';                                              \
    } while (0)
#define READ_NEXT(OnEOF)                                                       \
    do {                                                                       \
        if (*p == '\n') {                                                      \
            ++line;                                                            \
            linep = p + 1;                                                     \
        }                                                                      \
        if (++p >= map.end) {                                                  \
            OnEOF;                                                             \
        }                                                                      \
    } while (0)
#define READ_BLANK(OnEOF)                                                      \
    do {                                                                       \
        bool in_comment = false;                                               \
        while (in_comment || isspace(*p) || *p == '#') {                       \
            if (*p == '\n') {                                                  \
                in_comment = false;                                            \
            } else if (*p == '#') {                                            \
                in_comment = true;                                             \
            }                                                                  \
            READ_NEXT(OnEOF);                                                  \
        }                                                                      \
    } while (0)
#define READ_TOKEN(Name, Buffer, Len)                                          \
    do {                                                                       \
        (Len) = 0;                                                             \
        (Buffer)[0] = '\0';                                                    \
        if (!isalpha(*p)) {                                                    \
            READ_ERROR("invalid %s, unexpected character '%c'", Name, *p);     \
        }                                                                      \
        do {                                                                   \
            ADD_IN_BUFFER(Buffer, Len, *p);                                    \
            READ_NEXT(goto badeof);                                            \
        } while (isalnum(*p) || *p == '_');                                    \
    } while (0)
#define READ_STRING(Name, Buffer, Len, OnEOF)                                  \
    do {                                                                       \
        (Len) = 0;                                                             \
        (Buffer)[0] = '\0';                                                    \
        if (*p == '"') {                                                       \
            bool escaped = false;                                              \
            while (*p == '"') {                                                \
                READ_NEXT(goto badeof);                                        \
                while (true) {                                                 \
                    if (*p == '\n') {                                          \
                        READ_ERROR("string must not contain EOL");             \
                    } else if (escaped) {                                      \
                        ADD_IN_BUFFER(Buffer, Len, *p);                        \
                        escaped = false;                                       \
                    } else if (*p == '\\') {                                   \
                        escaped = true;                                        \
                    } else if (*p == '"') {                                    \
                        READ_NEXT(goto badeof);                                \
                        break;                                                 \
                    } else {                                                   \
                        ADD_IN_BUFFER(Buffer, Len, *p);                        \
                    }                                                          \
                    READ_NEXT(goto badeof);                                    \
                }                                                              \
                READ_BLANK(goto badeof);                                       \
            }                                                                  \
            if (*p != ';') {                                                   \
                READ_ERROR("%s must end with a ';'", Name);                    \
            }                                                                  \
        } else {                                                               \
            bool escaped = false;                                              \
            while (*p != ';' && isascii(*p) && (isprint(*p) || isspace(*p))) { \
                if (escaped) {                                                 \
                    if (*p == '\r' || *p == '\n') {                            \
                        READ_BLANK(goto badeof);                               \
                    } else {                                                   \
                        ADD_IN_BUFFER(Buffer, Len, '\\');                      \
                    }                                                          \
                    escaped = false;                                           \
                }                                                              \
                if (*p == '\\') {                                              \
                    escaped = true;                                            \
                } else if (*p == '\r' || *p == '\n') {                         \
                    READ_ERROR("%s must not contain EOL", Name);               \
                } else {                                                       \
                    ADD_IN_BUFFER(Buffer, Len, *p);                            \
                }                                                              \
                READ_NEXT(goto badeof);                                        \
            }                                                                  \
            if (escaped) {                                                     \
                ADD_IN_BUFFER(Buffer, Len, '\\');                              \
            }                                                                  \
            while ((Len) > 0 && isspace((Buffer)[(Len) - 1])) {                \
                (Buffer)[--(Len)] = '\0';                                      \
            }                                                                  \
        }                                                                      \
        READ_NEXT(OnEOF);                                                      \
    } while(0)


read_section:
    if (p >= map.end) {
        goto ok;
    }

    value[0] = key[0] = '\0';
    value_len = key_len = 0;

    READ_BLANK(goto ok);
    READ_TOKEN("section name", key, key_len);
    READ_BLANK(goto badeof);
    switch (*p) {
      case '=':
        READ_NEXT(goto badeof);
        goto read_param_value;
      case '{':
        READ_NEXT(goto badeof);
        goto read_filter;
      default:
        READ_ERROR("invalid character '%c', expected '=' or '{'", *p);
    }

read_param_value:
    READ_BLANK(goto badeof);
    READ_STRING("parameter value", value, value_len, ;);
    {
        filter_param_t param;
        param.type  = param_tokenize(key, key_len);
        if (param.type != ATK_UNKNOWN) {
            param.value     = p_dupstr(value, value_len);
            param.value_len = value_len;
            array_add(config->params, param);
        }
    }
    goto read_section;

read_filter:
    filter_set_name(&filter, key, key_len);
    READ_BLANK(goto badeof);
    while (*p != '}') {
        READ_TOKEN("filter parameter name", key, key_len);
        READ_BLANK(goto badeof);
        if (*p != '=') {
            READ_ERROR("invalid character '%c', expected '='", *p);
        }
        READ_NEXT(goto badeof);
        READ_BLANK(goto badeof);
        READ_STRING("filter parameter value", value, value_len, goto badeof);
        READ_BLANK(goto badeof);
        if (strcmp(key, "type") == 0) {
            if (!filter_set_type(&filter, value, value_len)) {
                READ_ERROR("unknow filter type (%s) for filter %s",
                           value, filter.name);
            }
        } else if (key_len > 3 && strncmp(key, "on_", 3) == 0) {
            if (!filter_add_hook(&filter, key + 3, key_len - 3,
                                 value, value_len)) {
                READ_ERROR("hook %s not supported by filter %s",
                           key + 3, filter.name);
            }
        } else {
            /* filter_add_param failure mean unknown type or unsupported type.
             * this are non-fatal errors.
             */
            (void)filter_add_param(&filter, key, key_len, value, value_len);
        }
    }
    READ_NEXT(;);
    if (!filter_build(&filter)) {
        READ_ERROR("invalid filter %s", filter.name);
    }
    array_add(config->filters, filter);
    filter_init(&filter);
    goto read_section;

ok:
    if (!config_second_pass(config)) {
        goto error;
    }
    file_map_close(&map);
    return true;

badeof:
    syslog(LOG_ERR, "Unexpected end of file");

error:
    if (filter.name) {
        filter_wipe(&filter);
    }
    file_map_close(&map);
    return false;
}

bool config_reload(config_t *config)
{
    return config_load(config);
}

config_t *config_read(const char *file)
{
    config_t *config = config_new();
    config->filename = file;
    if (!config_reload(config)) {
        p_delete(&config);
        return NULL;
    }
    return config;
}
