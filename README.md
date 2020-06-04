# gzd0.7 FPGA board test driver

This device driver code is for Gen-z 0.7 FPGA card board.
Original source code supported for linux kernel 4.4 and BIO(block IO) mode only.
ETRI modify to support Request mode in linux kernel with original vendor supplied code.
BIO mode can not select IO schedulers in linux kernel in sysfs.
Meanwhile request mode can use them(dead-line, cfq, noop).

## Requirement for test

### H/W
- Gen-Z 0.7 FPGA card
- Intel Xeon or AMD Threadripper
### S/W 
    1. Ubuntu 16.04 or 18.04
    2. linux kernel 4.4, 4.15 for each version.
    3. dkms installation
    4. /etc/modprobe.d/gzd.conf must be set to initialize the basic card setup.
    5. gzd.conf is supplied with this driver code in each version

## Procedure
   1. H/W System(Intel XEON above) setup and FPGA card install
   2. Install kernel, module and header with linux kernel 4.4 or 4.15 each(use Ubuntu defcofig)
   3. Download driver sources with git clone command & copy to /usr/src/
   4. Compile each driver (/usr/src/gzd-4.0rq/./install-dkms-4.0rq
   5. modprobe -a gzd-core(for gzd core install)
   6. modprobe -a gzd-stor(for gzd char & block device install)
   7. modprobe -a gzd-en(for network socket)
   8. You can see gzb0_0_0(block device) or gzc0_0_0(native char device) in /dev directory
   9. Start test
   
 We tested with Ubuntu 16.04, 18.04 with each linux kernel version.
 Please enjoy it.
