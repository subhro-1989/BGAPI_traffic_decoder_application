#ifndef BT_TRANSPORT_ADAPTER_H
#define BT_TRANSPORT_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* void(uint32_t len, uint8_t *data) -- matches tx_func in sl_bt_ncp_host.h */
void bt_transport_tx(uint32_t msg_len, uint8_t *msg_data);

#ifdef __cplusplus
}
#endif

#endif /* BT_TRANSPORT_ADAPTER_H */
