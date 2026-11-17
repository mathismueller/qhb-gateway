QHB_DECTD_VERSION = 1.0.0
QHB_DECTD_SITE = $(BR2_EXTERNAL_QHB_GATEWAY_PATH)/src/qhb-dectd
QHB_DECTD_SITE_METHOD = local

define QHB_DECTD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/hb2_dectd.c -o $(@D)/qhb-dectd
	$(TARGET_CC) -O2 -Wall -Wextra -shared -fPIC -nostdlib \
		$(@D)/libvoiper_shim.c -o $(@D)/libvoiper_shim.so
endef

define QHB_DECTD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/qhb-dectd $(TARGET_DIR)/usr/sbin/qhb-dectd
	$(INSTALL) -D -m 0755 $(@D)/libvoiper_shim.so $(TARGET_DIR)/lib/vendor/libvoiper_shim.so
endef

$(eval $(generic-package))
