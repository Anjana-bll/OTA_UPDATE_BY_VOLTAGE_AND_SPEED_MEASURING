/*
 * feature_engine.c
 *
 *  Created on: 06-Mar-2026
 *      Author: Anjana Roy
 */

#include "feature_engine.h"
#include <string.h>
 #include <stdio.h>

/* ===========================
   Local config
   =========================== */
#define FE_MAX_FEATURES      10
#define FE_MAX_ALIASES       3
#define FE_NAME_MAX          32

/* ===========================
   Normalization helpers
   =========================== */
/* Convert to uppercase AND strip non-alnum to make matching robust */
#if FE_USE_STORE
static void normalize(char *s)
{
    char *d = s;
    for (char *p = s; *p; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            *d++ = c;
    }
    *d = '\0';
}
#endif

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= ('a' - 'A');
        if (cb >= 'a' && cb <= 'z') cb -= ('a' - 'A');
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* ===========================
   Engine data
   =========================== */

#if FE_USE_TABLE
typedef struct {
    char          name[FE_NAME_MAX];
    char          aliases[FE_MAX_ALIASES][FE_NAME_MAX];
    feat_handler_t handler;
    uint8_t       enabled_once; /* becomes 1 if handler succeeded at least once */
} feat_entry_t;

static feat_entry_t g_table[FE_MAX_FEATURES];

#elif FE_USE_STORE
typedef struct {
    char     name[FE_NAME_MAX];
    uint8_t  enabled;
} feat_kv_t;

static feat_kv_t g_store[FE_MAX_FEATURES];
#endif

/* ===========================
   Public API
   =========================== */
void feature_engine_init(void)
{
#if FE_USE_TABLE
    memset(g_table, 0, sizeof(g_table));
#elif FE_USE_STORE
    memset(g_store, 0, sizeof(g_store));
#endif
}

#if FE_USE_TABLE
static int find_entry_index(const char *rawName)
{
    for (int i = 0; i < FE_MAX_FEATURES; ++i) {
        if (g_table[i].handler == NULL && g_table[i].name[0] == 0) continue;
        if (ci_equal(rawName, g_table[i].name)) return i;
        for (int a = 0; a < FE_MAX_ALIASES; ++a) {
            if (g_table[i].aliases[a][0] == 0) break;
            if (ci_equal(rawName, g_table[i].aliases[a])) return i;
        }
    }
    return -1;
}

feat_status_t feature_register(const char *name, feat_handler_t handler)
{
    if (!name || !handler) return FEAT_ERR_BADARG;

    /* Find empty slot */
    for (int i = 0; i < FE_MAX_FEATURES; ++i) {
        if (g_table[i].name[0] == 0 && g_table[i].handler == NULL) {
            /* Store canonical name as-is (but also keep normalized shadow to speed compares if you want) */
            strncpy(g_table[i].name, name, FE_NAME_MAX-1);
            g_table[i].name[FE_NAME_MAX-1] = '\0';
            g_table[i].handler = handler;
            g_table[i].enabled_once = 0;
            memset(g_table[i].aliases, 0, sizeof(g_table[i].aliases));
            return FEAT_OK;
        }
    }
    return FEAT_ERR_FULL;
}

feat_status_t feature_add_alias(const char *canonical, const char *alias)
{
    if (!canonical || !alias) return FEAT_ERR_BADARG;

    /* Find canonical */
    int idx = -1;
    for (int i = 0; i < FE_MAX_FEATURES; ++i) {
        if (g_table[i].handler == NULL && g_table[i].name[0] == 0) continue;
        if (ci_equal(canonical, g_table[i].name)) { idx = i; break; }
    }
    if (idx < 0) return FEAT_ERR_NOT_FOUND;

    /* Find alias slot */
    for (int a = 0; a < FE_MAX_ALIASES; ++a) {
        if (g_table[idx].aliases[a][0] == 0) {
            strncpy(g_table[idx].aliases[a], alias, FE_NAME_MAX-1);
            g_table[idx].aliases[a][FE_NAME_MAX-1] = '\0';
            return FEAT_OK;
        }
    }
    return FEAT_ERR_FULL;
}

#elif FE_USE_STORE

static int find_store_index(const char *norm)
{
    for (int i = 0; i < FE_MAX_FEATURES; ++i) {
        if (g_store[i].enabled && strcmp(g_store[i].name, norm) == 0) return i;
    }
    return -1;
}

static int insert_store(const char *norm)
{
    for (int i = 0; i < FE_MAX_FEATURES; ++i) {
        if (g_store[i].name[0] == 0) {
            strncpy(g_store[i].name, norm, FE_NAME_MAX-1);
            g_store[i].name[FE_NAME_MAX-1] = '\0';
            g_store[i].enabled = 1;
            return i;
        }
    }
    return -1;
}
#endif /* FE_USE_STORE */

feat_status_t feature_engine_handle_activation(const char *rawFeature,
                                               const char *nonce,
                                               char *ackBuf,
                                               size_t ackLen)
{
    if (!ackBuf || ackLen < 8) return FEAT_ERR_BADARG;
    if (!rawFeature || !nonce) {
        snprintf(ackBuf, ackLen, "<ACK,?,? ,ERROR_FORMAT>");
        return FEAT_ERR_BADARG;
    }

#if FE_USE_TABLE
    /* Find feature (by name or alias) */
    int idx = find_entry_index(rawFeature);
    if (idx < 0 || g_table[idx].handler == NULL) {
        snprintf(ackBuf, ackLen, "<ACK,%s,%s,ERROR_FEATURE>", rawFeature, nonce);
        return FEAT_ERR_NOT_FOUND;
    }

    feat_status_t st = g_table[idx].handler(nonce, rawFeature);
    if (st == FEAT_OK) {
        g_table[idx].enabled_once = 1;
        snprintf(ackBuf, ackLen, "<ACK,%s,%s,SUCCESS>", rawFeature, nonce);
        return FEAT_OK;
    } else {
        snprintf(ackBuf, ackLen, "<ACK,%s,%s,ERROR_FEATURE>", rawFeature, nonce);
        return st;
    }

#elif FE_USE_STORE
    /* Normalize and enable any feature name */
    char norm[FE_NAME_MAX];
    strncpy(norm, rawFeature, FE_NAME_MAX-1);
    norm[FE_NAME_MAX-1] = '\0';
    normalize(norm);

    if (norm[0] == 0) {
        snprintf(ackBuf, ackLen, "<ACK,%s,%s,ERROR_FORMAT>", rawFeature, nonce);
        return FEAT_ERR_BADARG;
    }

    int idx = find_store_index(norm);
    if (idx < 0) {
        int ins = insert_store(norm);
        if (ins < 0) {
            snprintf(ackBuf, ackLen, "<ACK,%s,%s,ERROR_FULL>", rawFeature, nonce);
            return FEAT_ERR_FULL;
        }
    }
    snprintf(ackBuf, ackLen, "<ACK,%s,%s,SUCCESS>", rawFeature, nonce);
    return FEAT_OK;
#endif
}

int feature_is_enabled_ci(const char *rawName)
{
    if (!rawName) return 0;

#if FE_USE_TABLE
    int idx = find_entry_index(rawName);
    if (idx < 0) return 0;
    return g_table[idx].enabled_once ? 1 : 0;

#elif FE_USE_STORE
    char norm[FE_NAME_MAX];
    strncpy(norm, rawName, FE_NAME_MAX-1);
    norm[FE_NAME_MAX-1] = '\0';
    normalize(norm);
    return (find_store_index(norm) >= 0);
#endif
}
