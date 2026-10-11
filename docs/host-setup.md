# Host Setup

## Linux

```bash
sudo pppd /dev/ttyACM0 115200 192.168.7.1:192.168.7.2 noauth local nodetach \
     novj noccp nodefaultroute ms-dns 8.8.8.8
ping 192.168.7.2
```

- To give the board internet access, enable forwarding and NAT on the computer: `sysctl -w net.ipv4.ip_forward=1` and `iptables -t nat -A POSTROUTING -o <internet-if> -j MASQUERADE`.
- ModemManager may probe new `ttyACM` devices with AT commands and get in the way. Stop it, or add a udev rule with `ENV{ID_MM_DEVICE_IGNORE}="1"`.
- `noauth` needs root. Without it, pppd asks the board to authenticate: set the credentials with `setCredentials()` and add them to `/etc/ppp/pap-secrets`.

## macOS

macOS ships `pppd` in `/usr/sbin`. The board's USB serial port shows up as `/dev/cu.usbmodem*` (list the ports with `ls /dev/cu.*`). Use the `cu.` device, not the `tty.` one: opening `tty.` waits for a carrier signal.

```bash
sudo pppd /dev/cu.usbmodem1101 115200 192.168.7.1:192.168.7.2 noauth local nodetach \
     novj noccp nodefaultroute ms-dns 8.8.8.8
ping 192.168.7.2
```

- To give the board internet access, enable forwarding (`sudo sysctl -w net.inet.ip.forwarding=1`). Then add a NAT rule for `pf` with your internet interface, for example `en0`:

  ```bash
  echo "nat on en0 from 192.168.7.0/24 to any -> (en0)" | sudo pfctl -ef -
  ```

  This replaces the active `pf` rules until the next reboot. Restore the defaults with `sudo pfctl -f /etc/pf.conf`.
- `noauth` needs root. Without it, pppd asks the board to authenticate: set the credentials with `setCredentials()` and add them to `/etc/ppp/pap-secrets`.
