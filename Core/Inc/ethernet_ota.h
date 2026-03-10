/* ethernet_ota.h */
#ifndef ETHERNET_OTA_H
#define ETHERNET_OTA_H

#include "lwip/tcp.h"
#include "stm32h7xx_hal.h"
#include <stdbool.h>

/* Defines */
#define OTA_RX_BUFFER_SIZE 10240

/* OTA connection structure */
typedef struct {
    struct tcp_pcb *pcb;
    uint8_t rx_buf[OTA_RX_BUFFER_SIZE];
    uint32_t rx_len;
    bool header_parsed;
    uint32_t hdr_len;
    uint32_t total_size;
    bool erase_done;
    uint32_t bytes_written;          // Physical flash bytes written (aligned to 32)
    uint32_t firmware_bytes_received; // ACTUAL firmware bytes received (NEW!)
    uint32_t flash_write_addr;
    uint32_t last_activity_tick;
} ota_conn_t;
/* Function prototypes */
void ethernet_ota_init(void);
err_t ethernet_ota_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
err_t ethernet_ota_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len);
void ethernet_ota_err_cb(void *arg, err_t err);
void ethernet_ota_periodic(void);

void set_current_ota(ota_conn_t *ota);
ota_conn_t *get_current_ota(void);

/* Web debug functions */
const char* get_debug_log(void);
void clear_debug_log(void);
void web_debug_log(const char *format, ...);

#endif /* ETHERNET_OTA_H */
