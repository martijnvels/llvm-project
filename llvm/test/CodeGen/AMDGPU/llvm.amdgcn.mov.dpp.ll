; RUN: llc -march=amdgcn -mcpu=tonga -mattr=-flat-for-global -verify-machineinstrs -show-mc-encoding < %s | FileCheck -check-prefixes=VI,VI-OPT,PREGFX10,PREGFX10-OPT %s
; RUN: llc -O0 -march=amdgcn -mcpu=tonga -mattr=-flat-for-global -verify-machineinstrs -show-mc-encoding < %s | FileCheck -check-prefixes=VI,VI-NOOPT,PREGFX10,PREGFX10-NOOPT %s
; RUN: llc -march=amdgcn -mcpu=gfx1010 -mattr=-flat-for-global -verify-machineinstrs -show-mc-encoding < %s | FileCheck -check-prefixes=VI,VI-OPT %s

; FIXME: The register allocator / scheduler should be able to avoid these hazards.

; VI-LABEL: {{^}}dpp_test:
; VI: v_mov_b32_e32 v0, s{{[0-9]+}}
; VI-NOOPT: v_mov_b32_e32 v1, s{{[0-9]+}}
; PREGFX10-OPT: s_nop 1
; PREGFX10-NOOPT: s_nop 0
; PREGFX10-NOOPT: s_nop 0
; VI-OPT: v_mov_b32_dpp v0, v0 quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0 ; encoding: [0xfa,0x02,0x00,0x7e,0x00,0x01,0x08,0x11]
; VI-NOOPT: v_mov_b32_dpp v0, v1 quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0 ; encoding: [0xfa,0x02,0x00,0x7e,0x01,0x01,0x08,0x11]
define amdgpu_kernel void @dpp_test(i32 addrspace(1)* %out, i32 %in) {
  %tmp0 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %in, i32 1, i32 1, i32 1, i1 1) #0
  store i32 %tmp0, i32 addrspace(1)* %out
  ret void
}

; VI-LABEL: {{^}}dpp_wait_states:
; VI-NOOPT: v_mov_b32_e32 [[VGPR1:v[0-9]+]], s{{[0-9]+}}
; VI: v_mov_b32_e32 [[VGPR0:v[0-9]+]], s{{[0-9]+}}
; PREGFX10-OPT: s_nop 1
; PREGFX10-NOOPT: s_nop 0
; PREGFX10-NOOPT: s_nop 0
; VI-OPT: v_mov_b32_dpp [[VGPR0]], [[VGPR0]] quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0
; VI-NOOPT: v_mov_b32_dpp [[VGPR1]], [[VGPR0]] quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:
; PREGFX10-OPT: s_nop 1
; PREGFX10-NOOPT: s_nop 0
; PREGFX10-NOOPT: s_nop 0
; VI-OPT: v_mov_b32_dpp v{{[0-9]+}}, [[VGPR0]] quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0
; VI-NOOPT: v_mov_b32_dpp v{{[0-9]+}}, [[VGPR1]] quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0
define amdgpu_kernel void @dpp_wait_states(i32 addrspace(1)* %out, i32 %in) {
  %tmp0 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %in, i32 1, i32 1, i32 1, i1 1) #0
  %tmp1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %tmp0, i32 1, i32 1, i32 1, i1 1) #0
  store i32 %tmp1, i32 addrspace(1)* %out
  ret void
}

; VI-LABEL: {{^}}dpp_first_in_bb:
; VI: ; %endif
; PREGFX10-OPT: s_mov_b32
; PREGFX10-OPT: s_mov_b32
; PREGFX10-NOOPT: s_waitcnt
; PREGFX10-NOOPT: v_mov_b32_e32
; PREGFX10-NOOPT-NEXT: s_nop 0
; VI: v_mov_b32_dpp [[VGPR0:v[0-9]+]], v{{[0-9]+}} quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0
; PREGFX10-OPT: s_nop 1
; VI: v_mov_b32_dpp [[VGPR1:v[0-9]+]], [[VGPR0]] quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0
; PREGFX10-OPT: s_nop 1
; PREGFX10-NOOPT: s_nop 0
; PREGFX10-NOOPT: s_nop 0
; VI: v_mov_b32_dpp v{{[0-9]+}}, [[VGPR1]] quad_perm:[1,0,0,0] row_mask:0x1 bank_mask:0x1 bound_ctrl:0
define amdgpu_kernel void @dpp_first_in_bb(float addrspace(1)* %out, float addrspace(1)* %in, float %cond, float %a, float %b) {
  %cmp = fcmp oeq float %cond, 0.0
  br i1 %cmp, label %if, label %else

if:
  %out_val = load float, float addrspace(1)* %out
  %if_val = fadd float %a, %out_val
  br label %endif

else:
  %in_val = load float, float addrspace(1)* %in
  %else_val = fadd float %b, %in_val
  br label %endif

endif:
  %val = phi float [%if_val, %if], [%else_val, %else]
  %val_i32 = bitcast float %val to i32
  %tmp0 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %val_i32, i32 1, i32 1, i32 1, i1 1) #0
  %tmp1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %tmp0, i32 1, i32 1, i32 1, i1 1) #0
  %tmp2 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %tmp1, i32 1, i32 1, i32 1, i1 1) #0
  %tmp_float = bitcast i32 %tmp2 to float
  store float %tmp_float, float addrspace(1)* %out
  ret void
}

declare i32 @llvm.amdgcn.mov.dpp.i32(i32, i32, i32, i32, i1) #0

attributes #0 = { nounwind readnone convergent }

