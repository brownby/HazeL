#!/bin/bash
export f00=HazeL_`date -jRu +%y%m%d_%H%M%S`.csv
#find the port
export ppp=`ls /dev/cu.usb*`
cat $ppp | tee -a $f00

