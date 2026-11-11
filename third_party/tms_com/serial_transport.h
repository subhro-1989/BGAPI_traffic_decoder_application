/***************************************************************************//**
 * serial_transport.h
 *
 * Win32 COM-port transport for a Silicon Labs BGAPI/BGLib NCP host.
 *
 * This implements exactly the three functions BGLib's host-side "uart glue"
 * expects (the same signatures used in Silicon Labs' example_host uart.c
 * for POSIX). Wire this .c/.h pair in place of the POSIX uart.c that ships
 * with the NCP host reference code and you don't need to touch anything
 * else in sl_bt_api.c / bglib.c.
 *
 * If your copy of the host library uses slightly different function names
 * (e.g. uartTx vs bglib_output), just rename the three functions below to
 * match -- the bodies don't need to change.
 ******************************************************************************/
#ifndef SERIAL_TRANSPORT_H
#define SERIAL_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/***************************************************************************//**
 * Open the COM port and configure it for BGAPI use.
 *
 * @param[in] port_name   e.g. "COM7" (for COM10+ use "\\\\.\\COM10")
 * @param[in] baud_rate   e.g. 115200
 * @param[in] rts_cts_flow_control  1 = hardware (RTS/CTS) flow control,
 *                                  0 = none.
 *                                  Silicon Labs' stock NCP example firmware
 *                                  has RTS/CTS flow control ENABLED by
 *                                  default -- confirm against your firmware
 *                                  build before disabling this.
 *
 * @return 0 on success, -1 on failure (check GetLastError()).
 ******************************************************************************/
int serial_transport_open(const char *port_name, uint32_t baud_rate, int rts_cts_flow_control);

/***************************************************************************//**
 * 2026-09: interactive startup support. These do NOT open a port -- they
 * let the caller discover what's available and stage a chosen
 * port/baud pair before serial_transport_open() (called later, from
 * ble_link_init()) actually opens it.
 ******************************************************************************/

/* Bare COM port name, e.g. "COM3" or "COM10" -- serial_transport_open()
 * already prepends the "\\.\" prefix internally, so scan results and
 * staged config are both stored in this plain form. */
#define SERIAL_PORT_NAME_LEN 16
#define SERIAL_MAX_SCAN_PORTS 32

/***************************************************************************//**
 * Scan HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM for COM ports the
 * OS currently has a driver loaded for (i.e. a device is physically
 * connected and enumerated -- unplugged/never-connected ports don't
 * appear here). Fills 'ports' with up to 'max_ports' NUL-terminated
 * names, most-recently-enumerated order as the registry returns them.
 *
 * @param[out] ports      caller-supplied array of at least 'max_ports'
 *                         entries, each SERIAL_PORT_NAME_LEN bytes.
 * @param[in]  max_ports  capacity of 'ports'.
 * @param[out] out_count  number of ports actually found. Zero is a valid,
 *                         non-error result -- it means no serial device is
 *                         currently connected, not that scanning failed.
 *
 * @return 0 on success (including zero found), -1 on a real enumeration
 * failure (couldn't open the registry key at all).
 ******************************************************************************/
int serial_transport_scan_ports(char ports[][SERIAL_PORT_NAME_LEN], int max_ports, int *out_count);

/***************************************************************************//**
 * Validate and stage a COM port name + baud rate for later use --
 * typically called right after the user picks one from a
 * serial_transport_scan_ports() result. Does not open anything.
 *
 * @param[in] port_name  bare name, e.g. "COM10" (no "\\.\" prefix).
 * @param[in] baud_rate  e.g. 115200; must be nonzero.
 *
 * @return 0 if the values look sane and were stored, -1 otherwise
 * (NULL/empty port name, name too long for SERIAL_PORT_NAME_LEN, or
 * baud_rate == 0).
 ******************************************************************************/
int serial_transport_set_config(const char *port_name, uint32_t baud_rate);

/***************************************************************************//**
 * Retrieve the most recently staged config.
 *
 * @param[out] port_out  buffer of at least port_cap bytes.
 * @param[in]  port_cap  capacity of port_out.
 * @param[out] baud_out  receives the staged baud rate.
 *
 * @return 0 if a config has been staged (via serial_transport_set_config())
 * and port_cap was big enough, -1 otherwise.
 ******************************************************************************/
int serial_transport_get_config(char *port_out, size_t port_cap, uint32_t *baud_out);

/***************************************************************************//**
 * Close the COM port.
 ******************************************************************************/
void serial_transport_close(void);

/***************************************************************************//**
 * Blocking write. Sends exactly len bytes.
 * @return 0 on success, -1 on failure.
 ******************************************************************************/
int32_t uartTx(uint32_t len, const uint8_t *data);

/***************************************************************************//**
 * Blocking read with an overall timeout. Reads up to len bytes into data.
 * @return number of bytes actually read (0 on timeout, -1 on hard error).
 ******************************************************************************/
int32_t uartRx(uint32_t len, uint8_t *data);

/***************************************************************************//**
 * Non-blocking peek: how many bytes are currently sitting in the OS input
 * buffer. BGLib uses this to decide whether a full BGAPI header/payload is
 * available yet before calling uartRx().
 ******************************************************************************/
int32_t uartRxPeek(void);

/***************************************************************************//**
 * Blocking delay, in milliseconds. Thin wrapper over Win32 Sleep().
 * Useful after serial_transport_open() to let the target finish rebooting
 * (many WSTK boards reset the target MCU on DTR toggle when the COM port
 * opens) before you send the first command.
 ******************************************************************************/
void delay_ms(uint32_t ms);

/***************************************************************************//**
 * Monotonic milliseconds since system start. Thin wrapper over
 * Win32 GetTickCount64(). Used for our own wall-clock timeouts around
 * SDK operations (scanning, connecting, GATT discovery) whose duration
 * the SDK itself does not bound.
 ******************************************************************************/
uint64_t get_tick_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_TRANSPORT_H */
