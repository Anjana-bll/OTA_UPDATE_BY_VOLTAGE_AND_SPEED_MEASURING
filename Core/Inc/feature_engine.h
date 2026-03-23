/*
 * feature_engine.h
 *
 *  Created on: 06-Mar-2026
 *      Author: Anjana Roy
 */

#ifndef INC_FEATURE_ENGINE_H_
#define INC_FEATURE_ENGINE_H_


#include <stdint.h>
#include <stddef.h>

/* ===========================
   Select implementation
   =========================== */
/* Set exactly one of these to 1 */
#define FE_USE_TABLE   1   /* Option 2: Feature Table + Handlers (default) */
#define FE_USE_STORE   0   /* Option 3: Dynamic KV Store (runtime-any feature) */

#if (FE_USE_TABLE + FE_USE_STORE) != 1
# error "Select exactly one: FE_USE_TABLE or FE_USE_STORE"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========= Common types ========= */

typedef enum {
    FEAT_OK = 0,
    FEAT_ERR = 1,
    FEAT_ERR_FULL = 2,
    FEAT_ERR_NOT_FOUND = 3,
    FEAT_ERR_BADARG = 4
} feat_status_t;

typedef feat_status_t (*feat_handler_t)(const char *nonce, const char *rawName);

/* ========= Public API ========= */

/**
 * @brief Initialize the feature engine.
 *  - Clears any runtime tables/stores.
 *  - In TABLE mode, does not pre-register anything; caller should register.
 */
void feature_engine_init(void);

/**
 * @brief Builds an ACK into ackBuf for an activation request.
 * Parses already-detected frame fields (featureId, nonce) and applies engine mode:
 *  - TABLE: finds a registered feature/alias → calls handler → SUCCESS/ERROR_FEATURE
 *  - STORE: normalizes and enables any feature → SUCCESS (or ERROR_FULL)
 *
 * @param rawFeature  original feature string from frame (not normalized)
 * @param nonce       original nonce string from frame
 * @param ackBuf      destination buffer for ACK string
 * @param ackLen      size of ackBuf
 * @return FEAT_OK on SUCCESS, otherwise appropriate error (also reflected in ACK).
 */
feat_status_t feature_engine_handle_activation(const char *rawFeature,
                                               const char *nonce,
                                               char *ackBuf,
                                               size_t ackLen);

/**
 * @brief Case-insensitive query: is a feature enabled?
 *  - TABLE mode: returns 1 if its handler was invoked successfully at least once.
 *  - STORE mode: returns 1 if the normalized name is present in store.
 */
int feature_is_enabled_ci(const char *rawName);

/**
 * @brief Mark a feature as disabled in TABLE mode (sets enabled_once=1).
 *        No-op / error for STORE mode.
 */
feat_status_t feature_mark_enabled_once_ci(const char *rawName);

/**
 * @brief Mark a feature as disabled in TABLE mode (sets enabled_once=0).
 *        No-op / error for STORE mode.
 */
feat_status_t feature_mark_disabled_once_ci(const char *rawName);

/* ====== Option 2: TABLE mode registration API ====== */
#if FE_USE_TABLE
/**
 * @brief Register a feature with a handler.
 * Names are matched case-insensitively.
 * @param name     canonical feature name (e.g., "ANTIPINCH")
 * @param handler  function to call on activation (must be non-NULL)
 * @return FEAT_OK or FEAT_ERR_FULL/FEAT_ERR_BADARG
 */
feat_status_t feature_register(const char *name, feat_handler_t handler);

/**
 * @brief Add up to 3 aliases for an already-registered canonical name.
 * @param canonical  the canonical name used in feature_register
 * @param alias      alias to accept on activation (case-insensitive)
 * @return FEAT_OK or FEAT_ERR_NOT_FOUND/FEAT_ERR_FULL
 */
feat_status_t feature_add_alias(const char *canonical, const char *alias);
#endif /* FE_USE_TABLE */

#ifdef __cplusplus
}
#endif


#endif /* INC_FEATURE_ENGINE_H_ */
