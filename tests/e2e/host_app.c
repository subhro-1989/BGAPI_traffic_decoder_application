/* Real BGLib host on a POSIX pty -- stands in for the TMS Configurator. */
#include "sl_bt_ncp_host.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
static int fd;
static void tx(uint32_t n, uint8_t *d) { (void)!write(fd, d, n); }
static int32_t rx(uint32_t n, uint8_t *d) { uint32_t got = 0; for (int i = 0; i < 2000 && got < n; i++) { ssize_t r = read(fd, d + got, n - got); if (r > 0) got += (uint32_t)r; else usleep(1000); } return got == n ? (int32_t)n : -1; }
static int32_t peek(void) { int n = 0; ioctl(fd, FIONREAD, &n); return n; }
int main(int c, char **v) {
  fd = open(v[1], O_RDWR | O_NOCTTY | O_NONBLOCK); struct termios t; tcgetattr(fd, &t); cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t);
  sl_bt_api_initialize_nonblock(tx, rx, peek);
  int bad = 0, calls = 0, events = 0; sl_bt_msg_t e;
  usleep(1500000);
  for (int i = 0; i < 25; i++) {
    bd_addr a; uint8_t ty; uint16_t mtu; size_t rl; uint8_t rb[64]; uint8_t um[3] = {0x01, 0x10, (uint8_t)i};
    bad += sl_bt_system_hello() != 0; calls++;
    bad += sl_bt_system_get_identity_address(&a, &ty) != 0; calls++;
    bad += sl_bt_gatt_set_max_mtu(247, &mtu) != 0; calls++;
    bad += sl_bt_user_message_to_target(3, um, sizeof rb, &rl, rb) != 0; calls++;
    while (sl_bt_pop_event(&e) == SL_STATUS_OK) { events++; if (!(e.header & 0x80)) bad++; }
    usleep(120000);
  }
  printf("host app: %d calls, %d non-OK or unexpected, %d events\n", calls, bad, events);
  return bad != 0;
}
