typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

// 1. SysV GVar stubs
int ATP_UTIL_GVarAttach(int id) {
    return 0; // Success
}

static uint8_t g_cfg_0x30001[65536] = {0};
static uint8_t g_cfg_0x30002[4096] = {0};
static uint8_t g_cfg_default[4096] = {0};

void* ATP_UTIL_GVarGetValue(unsigned int varId, int flag) {
    if (varId == 0x00030001) {
        // Enable DECT base service at offset 0x6a7c
        *(uint32_t*)(g_cfg_0x30001 + 0x6a7c) = 1;
        return (void*)g_cfg_0x30001;
    } else if (varId == 0x00030002) {
        return (void*)g_cfg_0x30002;
    }
    return (void*)g_cfg_default;
}

// 2. Hardware / equip stubs
int ATP_SYS_SetDectReset(void) {
    return 0;
}

int ATP_EQUIP_InEquipMode(void) {
    return 0;
}

// 3. Database init stub
int ATP_DBInit(int param) {
    return 0;
}
