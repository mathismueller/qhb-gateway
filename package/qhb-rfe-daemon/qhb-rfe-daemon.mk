QHB_RFE_DAEMON_VERSION = 1.0.0
QHB_RFE_DAEMON_SITE = $(BR2_EXTERNAL_QHB_GATEWAY_PATH)/src/qhb-rfe-daemon
QHB_RFE_DAEMON_SITE_METHOD = local

define QHB_RFE_DAEMON_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/hb2_rfe_daemon.c -o $(@D)/qhb-rfe-daemon
endef

define QHB_RFE_DAEMON_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/qhb-rfe-daemon $(TARGET_DIR)/usr/sbin/qhb-rfe-daemon
endef

$(eval $(generic-package))
