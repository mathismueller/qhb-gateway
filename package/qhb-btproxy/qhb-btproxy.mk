QHB_BTPROXY_VERSION = 1.0.0
QHB_BTPROXY_SITE = $(BR2_EXTERNAL_QHB_GATEWAY_PATH)/src/qhb-btproxy
QHB_BTPROXY_SITE_METHOD = local

define QHB_BTPROXY_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		STRIP="$(TARGET_STRIP)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra -O2" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define QHB_BTPROXY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hb2-btproxy $(TARGET_DIR)/usr/sbin/qhb-btproxy
endef

$(eval $(generic-package))
