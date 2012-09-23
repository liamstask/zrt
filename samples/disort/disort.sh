#!/bin/bash

./gendata.sh

${ZRT_ROOT}/ns_start.sh 21

#config for 10src, 10dst nodes
SRC_FIRST=1
SRC_LAST=10
DST_FIRST=1
DST_LAST=10

COUNTER=$SRC_FIRST
while [  $COUNTER -le $SRC_LAST ]; do
	gnome-terminal --geometry=80x20 -t "zerovm sortsrc$COUNTER.manifest" -x sh -c "${ZEROVM_ROOT}/zerovm -Mmanifest/sortsrc"$COUNTER".manifest -e -v4"
    let COUNTER=COUNTER+1 
done

COUNTER=$DST_FIRST
while [  $COUNTER -le $DST_LAST ]; do
    gnome-terminal --geometry=80x20 -t "zerovm sortdst$COUNTER.manifest" -x sh -c "${ZEROVM_ROOT}/zerovm -Mmanifest/sortdst"$COUNTER".manifest -e -v4"
    let COUNTER=COUNTER+1 
done

date > /tmp/time
${ZEROVM_ROOT}/zerovm -Mmanifest/sortman.manifest -e -v4
date >> /tmp/time

cat log/sortman.stderr.log
echo Manager node working time: 
cat /tmp/time

${ZRT_ROOT}/ns_stop.sh
