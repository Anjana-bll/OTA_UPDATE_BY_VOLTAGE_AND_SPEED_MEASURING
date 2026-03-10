/* tcpServerRAW.c - Complete OTA Server with Web Debug Console */

#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>
#include "ethernet_ota.h"
#include "stm32h7xx_hal.h"

#define ETX_APP_FLASH_ADDR 0x08040000U

/* External functions from ethernet_ota.c */
extern const char* get_debug_log(void);
extern void clear_debug_log(void);

/* Connection state enum */
typedef enum {
    CONN_STATE_IDLE,
    CONN_STATE_RECEIVING_GET,
    CONN_STATE_RECEIVING_POST,
    CONN_STATE_CLOSING
} conn_state_t;

/* TCP server connection struct */
struct tcp_server_conn {
    struct tcp_pcb *pcb;
    conn_state_t state;
    uint32_t bytes_sent;
};

/* HTML page with debug console */
static const char ota_html[] =
"<!DOCTYPE html><html><head><title>STM32 OTA Debug</title>"
"<style>"
"body{font-family:Arial;margin:20px;background:#1a1a1a;color:#fff;}"
"h1{color:#4CAF50;}"
"#debug{background:#000;color:#0f0;padding:10px;height:400px;overflow-y:scroll;"
"font-family:monospace;font-size:12px;border:2px solid #4CAF50;margin-top:20px;white-space:pre-wrap;}"
"button{background:#4CAF50;color:#fff;border:none;padding:10px 20px;font-size:16px;"
"cursor:pointer;margin:5px;border-radius:5px;}"
"button:hover{background:#45a049;}"
"#status{margin:10px 0;font-weight:bold;}"
"progress{width:100%;height:30px;margin:10px 0;}"
".error{color:#f44;}"
".success{color:#4f4;}"
"</style></head><body>"
"<h1>STM32 OTA Firmware Upload</h1>"
"<input type='file' id='fw'><br>"
"<button onclick='upload()'>Upload Firmware</button>"
"<button onclick='clearLog()'>Clear Log</button>"
"<button onclick='refresh()'>Refresh Debug</button>"
"<p id='status'>Ready</p>"
"<progress id='prog' value='0' max='100'></progress>"
"<h2>Debug Console:</h2>"
"<div id='debug'>Waiting for operations...</div>"
"<script>"
"let logDiv=document.getElementById('debug');"
"let statusDiv=document.getElementById('status');"
"let progBar=document.getElementById('prog');"
"let uploading=false;"
"async function upload(){"
"  const file=document.getElementById('fw').files[0];"
"  if(!file){alert('Please select a firmware file');return;}"
"  statusDiv.textContent='Uploading '+file.name+' ('+file.size+' bytes)...';"
"  statusDiv.className='';"
"  uploading=true;"
"  try{"
"    const resp=await fetch('/',{method:'POST',body:file,headers:{'Content-Type':'application/octet-stream'}});"
"    const text=await resp.text();"
"    if(resp.status===200){"
"      statusDiv.textContent='✓ Upload Complete! '+text;"
"      statusDiv.className='success';"
"    }else{"
"      statusDiv.textContent='✗ Upload Failed ('+resp.status+'): '+text;"
"      statusDiv.className='error';"
"    }"
"    uploading=false;"
"    setTimeout(updateLog,500);"
"  }catch(e){"
"    statusDiv.textContent='✗ Network Error: '+e;"
"    statusDiv.className='error';"
"    uploading=false;"
"  }"
"}"
"async function updateLog(){"
"  try{"
"    const resp=await fetch('/debug');"
"    const log=await resp.text();"
"    logDiv.textContent=log;"
"    logDiv.scrollTop=logDiv.scrollHeight;"
"  }catch(e){}"
"}"
"async function clearLog(){"
"  await fetch('/clear');"
"  logDiv.textContent='Log cleared.';"
"}"
"async function refresh(){"
"  await updateLog();"
"}"
"setInterval(async()=>{"
"  await updateLog();"
"  if(uploading){"
"    try{"
"      const resp=await fetch('/progress');"
"      const text=await resp.text();"
"      const match=text.match(/(\\d+)/);"
"      if(match){"
"        progBar.value=parseInt(match[1]);"
"        statusDiv.textContent='Uploading... '+text;"
"      }"
"    }catch(e){}"
"  }"
"},250);"
"</script></body></html>";

/* Forward declarations */
static void close_connection(struct tcp_pcb *tpcb, struct tcp_server_conn *conn);
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void tcp_err_cb(void *arg, err_t err);
static err_t tcp_poll_cb(void *arg, struct tcp_pcb *tpcb);

/* Close connection properly */
static void close_connection(struct tcp_pcb *tpcb, struct tcp_server_conn *conn)
{
    if (conn) conn->state = CONN_STATE_CLOSING;

    tcp_arg(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);

    err_t err = tcp_close(tpcb);
    if (err != ERR_OK) tcp_abort(tpcb);

    if (conn) mem_free(conn);
}

/* TCP error callback */
static void tcp_err_cb(void *arg, err_t err)
{
    struct tcp_server_conn *conn = (struct tcp_server_conn *)arg;
    printf("TCP error: %d\r\n", err);
    if (conn) mem_free(conn);
}

/* TCP poll callback */
static err_t tcp_poll_cb(void *arg, struct tcp_pcb *tpcb)
{
    struct tcp_server_conn *conn = (struct tcp_server_conn *)arg;
    if (!conn) {
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    if (conn->state == CONN_STATE_CLOSING) {
        close_connection(tpcb, conn);
        return ERR_OK;
    }
    return ERR_OK;
}

/* TCP sent callback */
static err_t tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    struct tcp_server_conn *conn = (struct tcp_server_conn *)arg;
    if (!conn) return ERR_OK;

    conn->bytes_sent += len;

    if (conn->state == CONN_STATE_RECEIVING_GET)
        close_connection(tpcb, conn);

    return ERR_OK;
}

/* TCP receive callback - SAFE VERSION */
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)err;

    struct tcp_server_conn *conn = (struct tcp_server_conn *)arg;
    if (!conn) {
        if(p) pbuf_free(p);
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    // Connection closed by remote
    if (!p) {
        close_connection(tpcb, conn);
        return ERR_OK;
    }

    // Use static buffer to avoid heap fragmentation
    static char recv_buffer[2048];
    uint16_t copy_len = (p->tot_len < sizeof(recv_buffer)-1) ? p->tot_len : sizeof(recv_buffer)-1;

    pbuf_copy_partial(p, recv_buffer, copy_len, 0);
    recv_buffer[copy_len] = '\0';

    // Acknowledge received data
    tcp_recved(tpcb, p->tot_len);

//    printf("RX: %d bytes\r\n", p->tot_len);

    // Parse HTTP request line
    char *line_end = strstr(recv_buffer, "\r\n");
    if (!line_end) {
        pbuf_free(p);
        return ERR_OK;
    }

    // Extract method and path
    char method[16] = {0};
    char path[128] = {0};

    char *space1 = strchr(recv_buffer, ' ');
    if (space1 && (space1 - recv_buffer) < 15) {
        size_t method_len = space1 - recv_buffer;
        memcpy(method, recv_buffer, method_len);
        method[method_len] = '\0';

        char *path_start = space1 + 1;
        char *space2 = strchr(path_start, ' ');
        if (space2 && (space2 - path_start) < 127) {
            size_t path_len = space2 - path_start;
            memcpy(path, path_start, path_len);
            path[path_len] = '\0';
        }
    }

//    printf("Method: '%s', Path: '%s'\r\n", method, path);

    /* ==================== GET HANDLERS ==================== */
    if (strcmp(method, "GET") == 0) {
        conn->state = CONN_STATE_RECEIVING_GET;

        /* GET /debug - Return debug log */
        if (strcmp(path, "/debug") == 0) {
            const char *log = get_debug_log();
            size_t log_len = strlen(log);

            // Use static buffer for response
            static char response[4096];
            int hdr_len = snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                (unsigned)log_len);

            // Copy log content
            if ((size_t)hdr_len + log_len < sizeof(response)) {
                memcpy(response + hdr_len, log, log_len);
                tcp_write(tpcb, response, hdr_len + log_len, TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);
            }

            pbuf_free(p);
            return ERR_OK;
        }

        /* GET /clear - Clear debug log */
        if (strcmp(path, "/clear") == 0) {
            clear_debug_log();

            const char *response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "Connection: close\r\n\r\n"
                "Log cleared.";

            tcp_write(tpcb, response, strlen(response), TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);
            pbuf_free(p);
            return ERR_OK;
        }

        /* GET /progress - Upload progress */
        if (strcmp(path, "/progress") == 0) {
            ota_conn_t *ota = get_current_ota();
            char msg[64];

            if (ota && ota->total_size > 0) {
                uint32_t percent = (ota->bytes_written * 100) / ota->total_size;
                snprintf(msg, sizeof(msg), "%lu%% (%lu/%lu bytes)",
                         percent, ota->bytes_written, ota->total_size);
            } else {
                snprintf(msg, sizeof(msg), "No upload");
            }

            char response[256];
            int resp_len = snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n%s",
                (unsigned)strlen(msg), msg);

            tcp_write(tpcb, response, resp_len, TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);
            pbuf_free(p);
            return ERR_OK;
        }

        /* GET / or /index.html - Main HTML page */
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            size_t html_len = strlen(ota_html);

            char header[256];
            int hdr_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                (unsigned)html_len);

            // Send header
            tcp_write(tpcb, header, hdr_len, TCP_WRITE_FLAG_COPY);

            // Send HTML body in manageable chunks
            size_t sent = 0;
            while (sent < html_len) {
                size_t chunk = html_len - sent;
                if (chunk > 1024) chunk = 1024;  // 1KB chunks

                // Check if buffer space available
                u16_t available = tcp_sndbuf(tpcb);
                if (available < chunk) {
                    tcp_output(tpcb);  // Flush and try again
                    continue;
                }

                err_t write_err = tcp_write(tpcb, ota_html + sent, chunk, TCP_WRITE_FLAG_COPY);
                if (write_err != ERR_OK) {
                    printf("Write error: %d\r\n", write_err);
                    break;
                }

                sent += chunk;
            }

            tcp_output(tpcb);
            printf("Sent %u bytes HTML\r\n", (unsigned)sent);

            pbuf_free(p);
            return ERR_OK;
        }

        /* GET /favicon.ico - Browser requests this */
        if (strcmp(path, "/favicon.ico") == 0) {
            const char *response =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            tcp_write(tpcb, response, strlen(response), TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);
            pbuf_free(p);
            return ERR_OK;
        }

        /* Unknown path - 404 */
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 9\r\n"
            "Connection: close\r\n\r\n"
            "Not Found";

        tcp_write(tpcb, not_found, strlen(not_found), TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
        pbuf_free(p);
        return ERR_OK;
    }

    /* ==================== POST HANDLER ==================== */
    if (strcmp(method, "POST") == 0) {
        conn->state = CONN_STATE_RECEIVING_POST;

        // Allocate OTA connection structure
        ota_conn_t *ota = (ota_conn_t *)mem_malloc(sizeof(ota_conn_t));
        if (!ota) {
            printf("OTA malloc failed!\r\n");
            pbuf_free(p);
            close_connection(tpcb, conn);
            return ERR_MEM;
        }

        memset(ota, 0, sizeof(*ota));
        ota->pcb = tpcb;
        ota->flash_write_addr = ETX_APP_FLASH_ADDR;
        ota->last_activity_tick = HAL_GetTick();

        // Switch to OTA callbacks
        set_current_ota(ota);
        tcp_arg(tpcb, ota);
        tcp_recv(tpcb, ethernet_ota_recv_cb);
        tcp_sent(tpcb, ethernet_ota_sent_cb);
        tcp_err(tpcb, ethernet_ota_err_cb);

        // Free server connection struct (OTA takes over)
        mem_free(conn);

        // Pass first packet to OTA handler
        printf("Switching to OTA mode\r\n");
        return ethernet_ota_recv_cb(ota, tpcb, p, ERR_OK);
    }

    /* Unknown method */
    printf("Unknown method: %s\r\n", method);

    const char *bad_request =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 11\r\n"
        "Connection: close\r\n\r\n"
        "Bad Request";

    tcp_write(tpcb, bad_request, strlen(bad_request), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    pbuf_free(p);
    close_connection(tpcb, conn);

    return ERR_OK;
}

/* TCP accept callback */
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    (void)err;

//    printf("New connection\r\n");

    struct tcp_server_conn *conn = (struct tcp_server_conn *)mem_malloc(sizeof(struct tcp_server_conn));
    if (!conn) {
        printf("Accept malloc failed!\r\n");
        tcp_abort(newpcb);
        return ERR_MEM;
    }

    memset(conn, 0, sizeof(*conn));
    conn->pcb = newpcb;
    conn->state = CONN_STATE_IDLE;

    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, tcp_recv_cb);
    tcp_sent(newpcb, tcp_sent_cb);
    tcp_err(newpcb, tcp_err_cb);
    tcp_poll(newpcb, tcp_poll_cb, 4);

    return ERR_OK;
}

/* TCP Server Initialization */
void tcp_server_init(void)
{
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        printf("TCP new failed\r\n");
        return;
    }

    ip_set_option(pcb, SOF_REUSEADDR);

    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        printf("TCP bind failed\r\n");
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen_with_backlog(pcb, 5);
    if (!pcb) {
        printf("TCP listen failed\r\n");
        return;
    }

    tcp_accept(pcb, tcp_accept_cb);

    printf("TCP OTA server started on port 80\r\n");
}
