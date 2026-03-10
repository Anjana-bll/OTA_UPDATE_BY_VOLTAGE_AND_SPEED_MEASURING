/* ethernet_ota.c - Fixed OTA Implementation */

#include "ethernet_ota.h"
#include "lwip/tcp.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "stm32h7xx_hal.h"
#include "cmsis_gcc.h"

#define ETX_APP_FLASH_ADDR 0x08040000U
#define FLASHWORD_SIZE 32U
#define DEBUG_LOG_SIZE 8192
#define MAX_CONTENT_LENGTH (512*1024)

extern volatile bool ota_upload_done;

static ota_conn_t *active_ota = NULL;
static char debug_log[DEBUG_LOG_SIZE];
static uint32_t debug_log_pos = 0;

void web_debug_log(const char *format, ...)
{
    char temp[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);

    if (len < 0) return;
    if (len >= (int)sizeof(temp)) len = sizeof(temp) - 1;

    printf("%.*s", len, temp);

    if (debug_log_pos + (uint32_t)len >= DEBUG_LOG_SIZE - 10) {
        uint32_t shift = DEBUG_LOG_SIZE / 2;
        memmove(debug_log, debug_log + shift, DEBUG_LOG_SIZE - shift);
        if (debug_log_pos > shift) debug_log_pos -= shift;
        else debug_log_pos = 0;
    }

    if (debug_log_pos + (uint32_t)len < DEBUG_LOG_SIZE - 1) {
        memcpy(debug_log + debug_log_pos, temp, (size_t)len);
        debug_log_pos += (uint32_t)len;
        debug_log[debug_log_pos] = '\0';
    }
}

const char* get_debug_log(void) { return debug_log; }
void clear_debug_log(void) { debug_log_pos = 0; debug_log[0] = '\0'; }
void set_current_ota(ota_conn_t *ota) { active_ota = ota; }
ota_conn_t *get_current_ota(void) { return active_ota; }

static inline uint32_t align_down_u32(uint32_t x, uint32_t a) { return x & ~(a - 1U); }
static inline uint32_t align_up_u32(uint32_t x, uint32_t a) { return (x + a - 1U) & ~(a - 1U); }

static uint32_t parse_content_length(const char *header, uint32_t header_len)
{
    (void)header_len;
    const char *cl_start = strstr(header, "Content-Length:");
    if (!cl_start) cl_start = strstr(header, "content-length:");
    if (cl_start) {
        cl_start += strlen("Content-Length:");
        while (*cl_start == ' ' || *cl_start == '\t') cl_start++;
        long val = atol(cl_start);
        if (val < 0) return 0;
        if ((uint32_t)val > MAX_CONTENT_LENGTH) return 0;
        return (uint32_t)val;
    }
    return 0;
}

static bool erase_application_flash(void)
{
    web_debug_log("ERASE: Starting...\r\n");

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1);

    FLASH_EraseInitTypeDef eraseInit;
    eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
    eraseInit.Banks = FLASH_BANK_1;
    eraseInit.Sector = FLASH_SECTOR_2;
    eraseInit.NbSectors = 6;
    eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    uint32_t sector_error = 0;

    __disable_irq();
    HAL_MPU_Disable();
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&eraseInit, &sector_error);
    HAL_FLASH_Lock();
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
    __enable_irq();

    if (status != HAL_OK) {
        web_debug_log("ERASE: FAILED status=%d sector_err=0x%08lX\r\n",
                       (int)status, (unsigned long)sector_error);
        return false;
    }

    web_debug_log("ERASE: SUCCESS!\r\n");
    return true;
}

static bool write_flash_word(uint32_t address, uint8_t *data)
{
    if (address & (FLASHWORD_SIZE - 1U)) {
        web_debug_log("WRITE: addr not aligned 0x%08lX\r\n", (unsigned long)address);
        return false;
    }

    uint32_t src_addr = (uint32_t)data;
    uint32_t src_base = align_down_u32(src_addr, 32U);
    uint32_t src_size = align_up_u32(FLASHWORD_SIZE + (src_addr - src_base), 32U);
    SCB_CleanDCache_by_Addr((uint32_t*)src_base, src_size);
    __DSB();

    __disable_irq();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1);
    HAL_FLASH_Unlock();

    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                                                   address, (uint32_t)data);

    HAL_FLASH_Lock();
    __enable_irq();

    if (status != HAL_OK) {
        web_debug_log("WRITE: HAL_FLASH_Program fail %d\r\n", (int)status);
        return false;
    }

    uint32_t dst_base = align_down_u32(address, 32U);
    uint32_t dst_size = align_up_u32(FLASHWORD_SIZE + (address - dst_base), 32U);
    SCB_InvalidateDCache_by_Addr((uint32_t*)dst_base, dst_size);
    __DSB();

    return true;
}

void ethernet_ota_init(void)
{
    clear_debug_log();
    web_debug_log("OTA: Initialized\r\n");
    active_ota = NULL;
}

err_t ethernet_ota_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)err;

    if (!arg) {
        if (p) pbuf_free(p);
        return ERR_ARG;
    }

    ota_conn_t *ota = (ota_conn_t *)arg;

    // Connection closed by client
    if (!p) {
        web_debug_log("RECV: Connection closed by client\r\n");

        // Check if we completed the upload
        if (ota->total_size > 0 && ota->bytes_written >= ota->total_size) {
            web_debug_log("RECV: Upload was complete before close\r\n");
        } else {
            web_debug_log("RECV: Connection closed prematurely! %lu/%lu bytes\r\n",
                         ota->bytes_written, ota->total_size);
        }

        set_current_ota(NULL);
        mem_free(ota);
        tcp_arg(tpcb, NULL);
        tcp_close(tpcb);
        return ERR_OK;
    }

    uint16_t total_len = p->tot_len;

    // Buffer overflow check
    if ((uint32_t)total_len + ota->rx_len > OTA_RX_BUFFER_SIZE) {
        pbuf_free(p);
        web_debug_log("RECV: Buffer overflow, dropping packet\r\n");
        return ERR_MEM;
    }

    // Copy data to buffer
    pbuf_copy_partial(p, ota->rx_buf + ota->rx_len, total_len, 0);
    ota->rx_len += total_len;
    pbuf_free(p);

    web_debug_log("RECV: +%u, buf=%lu\r\n", total_len, ota->rx_len);

    // Initialize flash address if needed
    if (ota->flash_write_addr == 0) {
        ota->flash_write_addr = ETX_APP_FLASH_ADDR;
    }

    // Parse HTTP header once
    if (!ota->header_parsed) {
        for (uint32_t i = 0; i + 3 < ota->rx_len; ++i) {
            if (ota->rx_buf[i] == '\r' && ota->rx_buf[i+1] == '\n' &&
                ota->rx_buf[i+2] == '\r' && ota->rx_buf[i+3] == '\n') {

                ota->hdr_len = i + 4;
                ota->header_parsed = true;

                char hdr[512];
                uint32_t copy_len = ota->hdr_len;
                if (copy_len >= sizeof(hdr)) copy_len = sizeof(hdr) - 1;
                memcpy(hdr, ota->rx_buf, copy_len);
                hdr[copy_len] = '\0';

                ota->total_size = parse_content_length(hdr, ota->hdr_len);
                web_debug_log("HEADER: size=%lu\r\n", ota->total_size);

                if (ota->total_size == 0) {
                    web_debug_log("ERROR: Invalid Content-Length!\r\n");
                    tcp_recved(tpcb, total_len);
                    return ERR_VAL;
                }

                break;
            }
        }

        if (!ota->header_parsed) {
            tcp_recved(tpcb, total_len);
            return ERR_OK;
        }
    }

    // Erase flash once
    if (!ota->erase_done) {
        if (!erase_application_flash()) {
            tcp_recved(tpcb, total_len);
            return ERR_MEM;
        }
        ota->erase_done = true;
    }

    // Process firmware data
    uint32_t data_start = ota->hdr_len;
    uint32_t data_avail = ota->rx_len - data_start;
    uint32_t chunks = data_avail / FLASHWORD_SIZE;

    // Write complete flash words
    for (uint32_t i = 0; i < chunks; i++) {
        uint32_t offset = data_start + (i * FLASHWORD_SIZE);
        uint32_t addr = ota->flash_write_addr + ota->bytes_written;

        if (!write_flash_word(addr, &ota->rx_buf[offset])) {
            tcp_recved(tpcb, total_len);
            web_debug_log("WRITE: failed at addr=0x%08lX\r\n", (unsigned long)addr);
            return ERR_MEM;
        }

        ota->bytes_written += FLASHWORD_SIZE;
    }

    if (chunks > 0) {
        web_debug_log("WRITE: %lu bytes, total=%lu/%lu\r\n",
                       (unsigned long)(chunks * FLASHWORD_SIZE),
                       (unsigned long)ota->bytes_written,
                       (unsigned long)ota->total_size);
    }

    // Keep leftover data in buffer
    uint32_t processed = data_start + (chunks * FLASHWORD_SIZE);
    uint32_t leftover = ota->rx_len - processed;

    if (leftover > 0) {
        memmove(ota->rx_buf, ota->rx_buf + processed, leftover);
        ota->rx_len = leftover;
    } else {
        ota->rx_len = 0;
    }

    ota->hdr_len = 0;
    tcp_recved(tpcb, total_len);

    // Check if upload is complete
    // We're done when we've received all the data bytes (not counting physical flash alignment)
    uint32_t total_received = ota->bytes_written + ota->rx_len;

    if (ota->total_size > 0 && total_received >= ota->total_size) {
        web_debug_log("COMPLETE: All data received (%lu bytes)\r\n", total_received);

        // Write final partial chunk if needed
        if (ota->rx_len > 0) {
            uint32_t remaining = ota->total_size - ota->bytes_written;
            if (remaining > 0 && remaining <= FLASHWORD_SIZE) {

                uint8_t pad[FLASHWORD_SIZE];
                memset(pad, 0xFF, FLASHWORD_SIZE);

                uint32_t to_copy = (remaining < ota->rx_len) ? remaining : ota->rx_len;
                memcpy(pad, ota->rx_buf, to_copy);

                uint32_t addr = ota->flash_write_addr + ota->bytes_written;
                if (write_flash_word(addr, pad)) {
                    web_debug_log("FINAL: wrote %lu bytes (padded to %u)\r\n",
                                   (unsigned long)to_copy, FLASHWORD_SIZE);
                    ota->bytes_written += FLASHWORD_SIZE;
                } else {
                    web_debug_log("FINAL: write failed\r\n");
                    return ERR_MEM;
                }
            }
        }

        // Clean and invalidate caches
        SCB_CleanInvalidateDCache();
        SCB_InvalidateICache();
        __DSB();
        __ISB();

        // Verify written firmware
        volatile uint32_t *app = (volatile uint32_t *)ETX_APP_FLASH_ADDR;
        web_debug_log("VERIFY: SP=0x%08lX RST=0x%08lX\r\n",
                       (unsigned long)app[0], (unsigned long)app[1]);

        uint32_t sp = app[0];
        uint32_t rst = app[1];
        bool ok = true;

        // Validate stack pointer (SRAM range for STM32H7)
        if (!(sp >= 0x24000000U && sp <= 0x24080000U)) {
            web_debug_log("VERIFY: Invalid SP\r\n");
            ok = false;
        }

        // Validate reset handler (Flash range + Thumb bit)
        if (!((rst >= 0x08000000U && rst <= 0x08100000U) && (rst & 1U))) {
            web_debug_log("VERIFY: Invalid RST\r\n");
            ok = false;
        }

        const char *resp;
        if (ok) {
            resp = "HTTP/1.1 200 OK\r\n\r\nComplete!";
            tcp_write(tpcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);

            // Give TCP time to send response before closing
            for(int i = 0; i < 10; i++) {
                tcp_output(tpcb);
            }

            tcp_close(tpcb);

            ota_upload_done = true;
            set_current_ota(NULL);

            web_debug_log("OTA: SUCCESSFUL! Ready to reset.\r\n");
        } else {
            resp = "HTTP/1.1 500 Internal Server Error\r\n\r\nVerification failed";
            tcp_write(tpcb, resp, strlen(resp), TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);
            tcp_close(tpcb);

            web_debug_log("OTA: Verification FAILED!\r\n");
        }
    }

    return ERR_OK;
}

err_t ethernet_ota_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)arg;
    (void)tpcb;
    (void)len;
    return ERR_OK;
}

void ethernet_ota_err_cb(void *arg, err_t err)
{
    ota_conn_t *ota = (ota_conn_t *)arg;
    if (ota) {
        web_debug_log("ERROR: TCP error %d (received %lu/%lu bytes)\r\n",
                       (int)err, ota->bytes_written, ota->total_size);
        set_current_ota(NULL);
        mem_free(ota);
    }
}

void ethernet_ota_periodic(void) { }
