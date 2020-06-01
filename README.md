# gzd0.7

This device driver code is for Gen-z 0.7 FPGA card board.
Original source code supported for linux kernel 4.4 and BIO(block IO) mode only.
ETRI modify to support Request mode in linux kernel with original vendor supplied code.
BIO mode can not select IO schedulers in linux kernel in sysfs.
Meanwhile request mode can use them(dead-line, cfq, noop).

# Requirement for test
 : Gen-Z 0.7 FPGA card
 : Ubuntu 16.04
 : linux kernel 4.4, 4.15 for each version.
 : dkms installation
 : /etc/modprobe.d/gzd.conf must be set to initialize the basic card setup.
     gzd.conf is supplied with this driver code each version

 We tested with Ubuntu 16.04 with each linux kernel version.
 Please enjoy it.
