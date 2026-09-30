// LeapVM：虛擬機看到的 CPU 功能
//
// Linux 開機時會讀「功能識別暫存器」決定要用哪些指令。大部分照實告訴它，
// 只有 SME（矩陣運算擴充）先藏起來：M4 以後的晶片才有，Linux 在虛擬機裡啟用它
// 需要額外處理一大塊暫存器狀態，目前還沒做。
#include "vm.h"

void cpu_features_apply(hv_vcpu_t vcpu) {
    uint64_t v;
    if (hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64PFR1_EL1, &v) == HV_SUCCESS)
        HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ID_AA64PFR1_EL1, v & ~(0xfULL << 24)));  // SME 欄位清成 0
}
