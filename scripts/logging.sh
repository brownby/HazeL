#!/bin/bash
#find the port
export ppp=`ls /dev/cu.usb*`
#read the file name
read f00 < $ppp
cat $ppp | tee -a $f00