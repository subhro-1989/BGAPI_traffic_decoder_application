/***************************************************************************//**
 * serial_transport.c
 *
 * Win32 COM-port transport for a Silicon Labs BGAPI/BGLib NCP host.
 * Build with MSVC (cl) or MinGW-w64 (x86_64-w64-mingw32-gcc), link -lsetupapi
 * is NOT required -- only the standard Win32 API (kernel32) is used.
 ******************************************************************************/

#include "compat.h"
#include "serial_transport.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Overall read timeout for uartRx(), in milliseconds.
 * BGAPI responses to the module are effectively instantaneous (<10ms);
 * this generous value just guards against a wedged link. */
#define RX_TOTAL_TIMEOUT_MS   2000

static HANDLE s_hComm = INVALID_HANDLE_VALUE;

/* ---------------------------------------------------------------------- */
int serial_transport_open(const char *port_name, uint32_t baud_rate, int rts_cts_flow_control)
{
    char device_path[64];
    DCB dcb;
    COMMTIMEOUTS timeouts;

    /* "\\.\COMx" form works for both COM1-9 and COM10+, so always use it. */
    _snprintf_s(device_path, sizeof(device_path), _TRUNCATE, "\\\\.\\%s", port_name);

    s_hComm = CreateFileA(device_path,
                           GENERIC_READ | GENERIC_WRITE,
                           0,                      /* no sharing */
                           NULL,                   /* default security */
                           OPEN_EXISTING,
                           0,                      /* no FILE_FLAG_OVERLAPPED: keep this synchronous/simple */
                           NULL);

    if (s_hComm == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "serial_transport_open: CreateFile(%s) failed, GetLastError=%lu\n",
                device_path, (unsigned long)GetLastError());
        return -1;
    }

    /* Recommended: big-enough OS buffers so a burst of BGAPI events
     * (e.g. scan reports) can't get dropped between our uartRx() calls. */
    SetupComm(s_hComm, 4096, 4096);

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(s_hComm, &dcb)) {
        fprintf(stderr, "serial_transport_open: GetCommState failed, GetLastError=%lu\n",
                (unsigned long)GetLastError());
        CloseHandle(s_hComm);
        s_hComm = INVALID_HANDLE_VALUE;
        return -1;
    }

    dcb.BaudRate = (DWORD)baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;

    if (rts_cts_flow_control) {
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl  = RTS_CONTROL_HANDSHAKE;
    } else {
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl  = RTS_CONTROL_ENABLE;
    }
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDtrControl     = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX           = FALSE;   /* no XON/XOFF */
    dcb.fInX            = FALSE;
    dcb.fNull           = FALSE;
    dcb.fAbortOnError   = FALSE;

    if (!SetCommState(s_hComm, &dcb)) {
        fprintf(stderr, "serial_transport_open: SetCommState failed, GetLastError=%lu\n",
                (unsigned long)GetLastError());
        CloseHandle(s_hComm);
        s_hComm = INVALID_HANDLE_VALUE;
        return -1;
    }

    /* Timeouts tuned for "poll-ish" blocking reads: ReadFile returns as
     * soon as at least 1 byte is available, or after RX_TOTAL_TIMEOUT_MS
     * with whatever (possibly zero) bytes it got. See MSDN COMMTIMEOUTS:
     * this combination (nonzero ReadIntervalTimeout + nonzero multiplier/
     * constant) yields "return early on data, else timeout". */
    timeouts.ReadIntervalTimeout         = 50;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = RX_TOTAL_TIMEOUT_MS;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant   = 500;
    if (!SetCommTimeouts(s_hComm, &timeouts)) {
        fprintf(stderr, "serial_transport_open: SetCommTimeouts failed, GetLastError=%lu\n",
                (unsigned long)GetLastError());
        CloseHandle(s_hComm);
        s_hComm = INVALID_HANDLE_VALUE;
        return -1;
    }

    PurgeComm(s_hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return 0;
}

/* ---------------------------------------------------------------------- */
/* 2026-09: interactive startup support (scan / stage config). These use
 * only the registry APIs already pulled in via <windows.h> above -- no
 * extra library dependency (no setupapi.lib). */

static char    s_staged_port[SERIAL_PORT_NAME_LEN];
static uint32_t s_staged_baud = 0;
static int     s_has_staged_config = 0;

int serial_transport_scan_ports(char ports[][SERIAL_PORT_NAME_LEN], int max_ports, int *out_count)
{
    HKEY hKey;
    LONG rc;
    DWORD index;
    int count = 0;

    if (ports == NULL || max_ports <= 0 || out_count == NULL) {
        return -1;
    }
    *out_count = 0;

    rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                        0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS) {
        /* No serial devices have ever been enumerated on this system (or
         * this process lacks permission to read the key). Not treated as
         * a hard failure -- the caller sees *out_count == 0 and prompts
         * the user to connect a device, same as a clean "nothing found". */
        return 0;
    }

    for (index = 0; count < max_ports; index++) {
        char value_name[256];
        DWORD value_name_len = (DWORD)sizeof(value_name);
        char data[SERIAL_PORT_NAME_LEN];
        DWORD data_len = (DWORD)sizeof(data);
        DWORD type = 0;

        rc = RegEnumValueA(hKey, index, value_name, &value_name_len,
                           NULL, &type, (LPBYTE)data, &data_len);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (rc == ERROR_MORE_DATA) {
            /* Port name longer than SERIAL_PORT_NAME_LEN-1 -- vanishingly
             * unlikely ("COM" + digits), but skip rather than truncate a
             * name we'd then fail to open correctly. */
            continue;
        }
        if (rc != ERROR_SUCCESS) {
            continue; /* skip a malformed entry, keep scanning the rest */
        }
        if (type == REG_SZ && data_len > 0) {
            size_t copy_len = ((size_t)data_len < sizeof(data)) ? (size_t)data_len : sizeof(data) - 1u;
            /* RegEnumValueA's data_len sometimes includes the trailing
             * NUL and sometimes doesn't, depending on how the value was
             * originally written -- always terminate explicitly rather
             * than trust it. */
            memcpy(ports[count], data, copy_len);
            ports[count][copy_len] = '\0';
            count++;
        }
    }

    RegCloseKey(hKey);
    *out_count = count;
    return 0;
}

int serial_transport_set_config(const char *port_name, uint32_t baud_rate)
{
    size_t len;

    if (port_name == NULL || baud_rate == 0u) {
        return -1;
    }
    len = strlen(port_name);
    if (len == 0u || len >= SERIAL_PORT_NAME_LEN) {
        return -1;
    }
    memcpy(s_staged_port, port_name, len + 1u);
    s_staged_baud = baud_rate;
    s_has_staged_config = 1;
    return 0;
}

int serial_transport_get_config(char *port_out, size_t port_cap, uint32_t *baud_out)
{
    size_t needed;

    if (!s_has_staged_config || port_out == NULL || baud_out == NULL) {
        return -1;
    }
    needed = strlen(s_staged_port) + 1u;
    if (port_cap < needed) {
        return -1;
    }
    memcpy(port_out, s_staged_port, needed);
    *baud_out = s_staged_baud;
    return 0;
}

/* ---------------------------------------------------------------------- */
void serial_transport_close(void)
{
    if (s_hComm != INVALID_HANDLE_VALUE) {
        CloseHandle(s_hComm);
        s_hComm = INVALID_HANDLE_VALUE;
    }
}

/* ---------------------------------------------------------------------- */
int32_t uartTx(uint32_t len, const uint8_t *data)
{
    DWORD written = 0;

    if (s_hComm == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (!WriteFile(s_hComm, data, (DWORD)len, &written, NULL)) {
        fprintf(stderr, "uartTx: WriteFile failed, GetLastError=%lu\n",
                (unsigned long)GetLastError());
        return -1;
    }
    if (written != len) {
        fprintf(stderr, "uartTx: short write (%lu of %u bytes)\n",
                (unsigned long)written, len);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
int32_t uartRx(uint32_t len, uint8_t *data)
{
    DWORD read_bytes = 0;

    if (s_hComm == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (!ReadFile(s_hComm, data, (DWORD)len, &read_bytes, NULL)) {
        fprintf(stderr, "uartRx: ReadFile failed, GetLastError=%lu\n",
                (unsigned long)GetLastError());
        return -1;
    }

    /* IMPORTANT: the SDK's sl_bt_ncp_host.c only checks "ret < 0" to detect
     * failure -- it assumes a non-negative return means the FULL requested
     * byte count was delivered (the POSIX blocking-read() contract its
     * reference uart.c was written against). Our Win32 ReadFile can return
     * fewer bytes than requested on a plain timeout without that being a
     * real error. Reporting a partial count as "success" would let the SDK
     * treat uninitialized/stale bytes in `data` as real protocol data,
     * corrupting parsing. So: only report success on an exact full read;
     * anything else becomes -1, which the SDK already retries correctly. */
    if (read_bytes != (DWORD)len) {
        return -1;
    }
    return (int32_t)read_bytes;
}

/* ---------------------------------------------------------------------- */
int32_t uartRxPeek(void)
{
    COMSTAT comstat;
    DWORD errors;

    if (s_hComm == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (!ClearCommError(s_hComm, &errors, &comstat)) {
        fprintf(stderr, "uartRxPeek: ClearCommError failed, GetLastError=%lu\n",
                (unsigned long)GetLastError());
        return -1;
    }
    return (int32_t)comstat.cbInQue;
}

/* ---------------------------------------------------------------------- */
void delay_ms(uint32_t ms)
{
    Sleep((DWORD)ms);
}

/* ---------------------------------------------------------------------- */
uint64_t get_tick_ms(void)
{
    return (uint64_t)GetTickCount64();
}
