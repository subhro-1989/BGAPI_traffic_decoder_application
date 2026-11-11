/* bt_transport_adapter.c
 *
 * Bridges our Win32 serial_transport.c functions to the exact function
 * pointer signatures sl_bt_ncp_host.h wants for sl_bt_api_initialize_nonblock():
 *
 *   typedef void    (*tx_func)(uint32_t msg_len, uint8_t* msg_data);
 *   typedef int32_t (*rx_func)(uint32_t dataLength, uint8_t* data);
 *   typedef int32_t (*rx_peek_func)(void);
 *
 * Our uartRx/uartRxPeek already match rx_func/rx_peek_func exactly.
 * Only uartTx needs adapting: it returns int32_t (0/-1), but tx_func
 * must return void.
 */
#include "sl_bt_ncp_host.h"
#include "serial_transport.h"

#include <stdio.h>

/* Matches tx_func exactly: void(uint32_t, uint8_t*) */
void bt_transport_tx(uint32_t msg_len, uint8_t *msg_data)
{
    if (uartTx(msg_len, msg_data) != 0) {
        /* sl_bt_host_handle_command() has no way to propagate a TX failure
         * back to the caller (tx_func returns void), so at minimum surface
         * it loudly. A production app may want to set a global fault flag
         * here and check it after every sl_bt_* call instead. */
        fprintf(stderr, "bt_transport_tx: uartTx failed (%u bytes)\n", msg_len);
    }
}

/* uartRx and uartRxPeek already match rx_func / rx_peek_func exactly --
 * no adapter needed, pass them straight to sl_bt_api_initialize_nonblock(). */
