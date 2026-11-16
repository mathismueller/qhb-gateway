QHB_ZIGBEED_VERSION = 1.0.0
QHB_ZIGBEED_SITE = $(BR2_EXTERNAL_QHB_GATEWAY_PATH)/src/qhb-zigbeed
QHB_ZIGBEED_SITE_METHOD = local

define QHB_ZIGBEED_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/hb2-zigbeed.c -o $(@D)/qhb-zigbeed
endef

define QHB_ZIGBEED_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/qhb-zigbeed $(TARGET_DIR)/usr/sbin/qhb-zigbeed
endef

$(eval $(generic-package))
