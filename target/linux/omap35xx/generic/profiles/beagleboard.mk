#
# Copyright (C) 2006 - 2009 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

define Profile/Beagleboard
  NAME:=beagleboard
  FEATURES:=ext2 usb
endef

define Profile/Beagleboard/Description
        Beagleboard - www.beagleboard.org
endef
$(eval $(call Profile,Beagleboard))
