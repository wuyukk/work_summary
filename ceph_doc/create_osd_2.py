#!/usr/bin/env python
# coding=utf-8

#print_xattr.py

import os
import sys
import re
from configobj import ConfigObj, ConfigObjError



def main():
    if len(sys.argv) < 2 :
        print "create_osd.py osd_num,osd_num(ex:create_osd.py 0,1,2)"
        return
    conf_file = "/etc/ceph/ceph.conf"
    if len(sys.argv) >= 3:
        conf_file = sys.argv[2]
    try:
        conf = ConfigObj(conf_file)
    except ConfigObjError as err:
        print "Get config: error: %s" % err
        return -1
    start_end = map(int,sys.argv[1].split(","))
    for index in start_end:
        key = "osd." + str(index)
        host = conf[key]["public addr"]
        mount_point = "/mnt/osd" + str(index)
        cmd = "ssh %s rm -rf %s/*" % (host, mount_point)
        #cmd = "ssh %s ceph osd create" % host
        os.system(cmd)
        cmd = "ssh %s ceph-osd -i %d  --mkfs --mkkey" % (host, index)
        os.system(cmd)
            



if __name__ == "__main__":
    sys.exit(main())

