/*
 * nvm_lite.c
 *
 *  Created on: 11-Mar-2026
 *      Author: Anjana Roy
 */

#include "nvm_lite.h"
#include <string.h>

/* ---- Page header ---- (32B) */
#define PAGE_HDR_MAGIC  (0x50474844UL) /* 'PGHD' */
typedef struct {
    uint32_t magic;
    uint32_t page_seq;
    uint32_t reserved[5];
    uint32_t crc32;               /* CRC32 over first 28 bytes */
} page_header_t;

#define FLASHWORD_SIZE_BYTES (32U)

typedef struct {
    uint32_t base;
    uint32_t other_base;
    uint32_t next_off;
    uint32_t page_seq;
} nvm_ctx_t;

static nvm_ctx_t g_ctx;

/* ===== CRC32 (reflected) ===== */
static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t m = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & m);
        }
    }
    return ~crc;
}

uint32_t nvm_crc32_16B(const nvm_record_t *rec)
{
    return crc32_update(0xFFFFFFFFU, (const uint8_t*)rec, 16);
}

static uint32_t crc32_page_header(const page_header_t *ph)
{
    return crc32_update(0xFFFFFFFFU, (const uint8_t*)ph, sizeof(*ph) - 4);
}

static int is_erased32(const void *p32)
{
    const uint32_t *p = (const uint32_t*)p32;
    for (size_t i = 0; i < FLASHWORD_SIZE_BYTES/4; ++i)
        if (p[i] != 0xFFFFFFFFU) return 0;
    return 1;
}

/* ===== Flash helpers ===== */
static HAL_StatusTypeDef flash_erase_sector(uint32_t sector, uint32_t bank)
{
    FLASH_EraseInitTypeDef ei = {0};
    uint32_t sector_err = 0;
    HAL_StatusTypeDef st;

    HAL_FLASH_Unlock();
    ei.TypeErase    = FLASH_TYPEERASE_SECTORS;
    ei.Banks        = bank;
    ei.Sector       = sector;
    ei.NbSectors    = 1;
    ei.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    st = HAL_FLASHEx_Erase(&ei, &sector_err);
    HAL_FLASH_Lock();
    return st;
}

/* H7: program 32B flash word */
static HAL_StatusTypeDef flash_program_32B(uint32_t dst_addr, const void *src32B)
{
    HAL_StatusTypeDef st;
    HAL_FLASH_Unlock();
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, dst_addr, (uint32_t)src32B);
    HAL_FLASH_Lock();
    return st;
}

/* ===== Page management ===== */
static int page_header_read(uint32_t base, page_header_t *out)
{
    memcpy(out, (const void*)base, sizeof(*out));
    if (out->magic != PAGE_HDR_MAGIC) return 0;
    if (out->crc32 != crc32_page_header(out)) return 0;
    return 1;
}

static HAL_StatusTypeDef page_header_write(uint32_t base, uint32_t seq)
{
    page_header_t ph = {0};
    ph.magic = PAGE_HDR_MAGIC;
    ph.page_seq = seq;
    ph.crc32 = crc32_page_header(&ph);
    return flash_program_32B(base, &ph);
}

static int rec_is_valid(const nvm_record_t *r)
{
    if (r->magic != NVM_MAGIC) return 0;
    if (r->version != NVM_VERSION) return 0;
    if (r->crc32 != nvm_crc32_16B(r)) return 0;
    return 1;
}

/* Scan page for last valid record */
static uint32_t page_find_last_off(uint32_t base, nvm_record_t *last)
{
    uint32_t off = sizeof(page_header_t);
    uint32_t last_off = 0;
    nvm_record_t tmp;

    while (off + sizeof(nvm_record_t) <= NVM_SECTOR_SIZE) {
        memcpy(&tmp, (const void*)(base + off), sizeof(tmp));
        if (is_erased32(&tmp)) break;   /* first empty slot */
        if (rec_is_valid(&tmp)) {
            *last = tmp;
            last_off = off;
        }
        off += sizeof(nvm_record_t);
    }
    return last_off;
}

static HAL_StatusTypeDef page_erase_and_init(uint32_t base, uint32_t sector, uint32_t bank, uint32_t seq)
{
    HAL_StatusTypeDef st = flash_erase_sector(sector, bank);
    if (st != HAL_OK) return st;
    return page_header_write(base, seq);
}

static HAL_StatusTypeDef nvm_select_or_init_pages(void)
{
    page_header_t h0 = {0}, h1 = {0};
    int v0 = page_header_read(NVM_PAGE0_ADDR, &h0);
    int v1 = page_header_read(NVM_PAGE1_ADDR, &h1);

    if (!v0 && !v1) {
        /* Fresh: init page0 seq=1, keep page1 erased */
        HAL_StatusTypeDef st = page_erase_and_init(NVM_PAGE0_ADDR, NVM_SECTOR0, NVM_BANK, 1);
        if (st != HAL_OK) return st;
        (void)flash_erase_sector(NVM_SECTOR1, NVM_BANK);
        g_ctx.base = NVM_PAGE0_ADDR;
        g_ctx.other_base = NVM_PAGE1_ADDR;
        g_ctx.page_seq = 1;
        g_ctx.next_off = sizeof(page_header_t);
        return HAL_OK;
    }

    if (v0 && (!v1 || h0.page_seq >= h1.page_seq)) {
        g_ctx.base = NVM_PAGE0_ADDR;
        g_ctx.other_base = NVM_PAGE1_ADDR;
        g_ctx.page_seq = h0.page_seq;
    } else {
        g_ctx.base = NVM_PAGE1_ADDR;
        g_ctx.other_base = NVM_PAGE0_ADDR;
        g_ctx.page_seq = h1.page_seq;
    }

    nvm_record_t last;
    uint32_t loff = page_find_last_off(g_ctx.base, &last);
    g_ctx.next_off = (loff == 0) ? sizeof(page_header_t) : (loff + sizeof(nvm_record_t));
    return HAL_OK;
}

static HAL_StatusTypeDef nvm_rollover_and_write(const nvm_record_t *rec)
{
    uint32_t new_base   = g_ctx.other_base;
    uint32_t new_sector = (new_base == NVM_PAGE0_ADDR) ? NVM_SECTOR0 : NVM_SECTOR1;

    HAL_StatusTypeDef st = page_erase_and_init(new_base, new_sector, NVM_BANK, g_ctx.page_seq + 1);
    if (st != HAL_OK) return st;

    st = flash_program_32B(new_base + sizeof(page_header_t), rec);
    if (st != HAL_OK) return st;

    /* Erase old page to become 'other' */
    uint32_t old_base   = g_ctx.base;
    uint32_t old_sector = (old_base == NVM_PAGE0_ADDR) ? NVM_SECTOR0 : NVM_SECTOR1;
    (void)flash_erase_sector(old_sector, NVM_BANK);

    g_ctx.other_base = old_base;
    g_ctx.base       = new_base;
    g_ctx.page_seq  += 1;
    g_ctx.next_off   = sizeof(page_header_t) + sizeof(nvm_record_t);
    return HAL_OK;
}

/* ===== Public API ===== */
HAL_StatusTypeDef nvm_init(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    return nvm_select_or_init_pages();
}

int nvm_read_latest(nvm_record_t *out)
{
    nvm_record_t best = {0}, tmp = {0};
    uint32_t off0 = page_find_last_off(NVM_PAGE0_ADDR, &tmp);
    if (off0) best = tmp;
    uint32_t off1 = page_find_last_off(NVM_PAGE1_ADDR, &tmp);
    if (off1) {
        if (g_ctx.base == NVM_PAGE1_ADDR) best = tmp;
        else if (!off0) best = tmp;
    }
    if (rec_is_valid(&best)) { *out = best; return 1; }
    return 0;
}

HAL_StatusTypeDef nvm_write(const nvm_record_t *rec_in)
{
    nvm_record_t rec = *rec_in;
    rec.magic = NVM_MAGIC;
    rec.version = NVM_VERSION;
    rec.crc32 = nvm_crc32_16B(&rec);

    if (g_ctx.next_off + sizeof(nvm_record_t) <= NVM_SECTOR_SIZE) {
        HAL_StatusTypeDef st = flash_program_32B(g_ctx.base + g_ctx.next_off, &rec);
        if (st == HAL_OK) g_ctx.next_off += sizeof(nvm_record_t);
        return st;
    } else {
        return nvm_rollover_and_write(&rec);
    }
}

/* Optional: erase both NVM pages and re-init */
HAL_StatusTypeDef nvm_factory_reset(void)
{
    HAL_StatusTypeDef st0 = flash_erase_sector(NVM_SECTOR0, NVM_BANK);
    HAL_StatusTypeDef st1 = flash_erase_sector(NVM_SECTOR1, NVM_BANK);
    if (st0 != HAL_OK || st1 != HAL_OK) return HAL_ERROR;

    /* Recreate page0 header, seq=1 */
    HAL_StatusTypeDef st = page_erase_and_init(NVM_PAGE0_ADDR, NVM_SECTOR0, NVM_BANK, 1);
    if (st != HAL_OK) return st;

    g_ctx.base = NVM_PAGE0_ADDR;
    g_ctx.other_base = NVM_PAGE1_ADDR;
    g_ctx.page_seq = 1;
    g_ctx.next_off = sizeof(page_header_t);
    return HAL_OK;
}
