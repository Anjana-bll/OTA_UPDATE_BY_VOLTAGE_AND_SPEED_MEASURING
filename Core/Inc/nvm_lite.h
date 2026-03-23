/*
 * nvm_lite.h
 *
 *  Created on: 11-Mar-2026
 *      Author: Anjana Roy
 */

#ifndef INC_NVM_LITE_H_
#define INC_NVM_LITE_H_

#pragma once
#include "stm32h7xx_hal.h"
#include <stdint.h>
#include <stddef.h>

/* ================== H723ZGTX NVM placement ==================
 * Your linker uses FLASH 0x08000000..0x0803FFFF (256 KB).
 * The H723ZGTX has 1 MB Flash. We'll use the last 256 KB for NVM:
 *   Sector 6: 0x080C0000 (128 KB)
 *   Sector 7: 0x080E0000 (128 KB)
 * Do NOT place code there; only NVM records will be written.
 */
#define NVM_PAGE0_ADDR   (0x080C0000UL)   /* sector 6 */
#define NVM_PAGE1_ADDR   (0x080E0000UL)   /* sector 7 */

#define NVM_BANK         FLASH_BANK_1
#define NVM_SECTOR0      FLASH_SECTOR_6
#define NVM_SECTOR1      FLASH_SECTOR_7
#define NVM_SECTOR_SIZE  (128U * 1024U)

/* ===== Lifetime feature flags record (32 bytes) ===== */
#define NVM_MAGIC        (0xD0E0A55AU)
#define NVM_VERSION      (1U)

/* Feature bits */
#define FEAT_ANTIPINCH_EN   (1U << 0)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t feature_flags;  /* bit0 = ANTIPINCH */
    uint32_t reserved0;      /* keep 32 bytes total */
    uint32_t crc32;          /* CRC32 over first 16 bytes */
    uint32_t reserved[3];
} nvm_record_t;

/* ===== Public API ===== */
HAL_StatusTypeDef nvm_init(void);
int  nvm_read_latest(nvm_record_t *out);
HAL_StatusTypeDef nvm_write(const nvm_record_t *rec_in);

/* Optional maintenance (factory reset) */
HAL_StatusTypeDef nvm_factory_reset(void);

/* Utility: CRC over first 16 bytes */
uint32_t nvm_crc32_16B(const nvm_record_t *rec);

#endif /* INC_NVM_LITE_H_ */
