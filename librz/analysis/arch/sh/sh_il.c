// SPDX-FileCopyrightText: 2022 Dhruv Maroo <dhruvmaru007@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-only

#include "sh_il.h"
#include <rz_il/rz_il_opbuilder_begin.h>
#include "../../../asm/arch/sh/regs.h"

/**
 * \file sh_il.c
 *
 * Converts SuperH-4 instructions to RzIL statements
 * References:
 *  - https://www.st.com/resource/en/user_manual/cd00147165-sh-4-32-bit-cpu-core-architecture-stmicroelectronics.pdf (SH-4 32-bit architecture manual)
 *  - https://www.renesas.com/in/en/document/mas/sh-4-software-manual?language=en (SH-4 manual by Renesas)
 *
 * Both the above references are almost the same
 */

#define SH_U_ADDR(x) UN(SH_ADDR_SIZE, x)
#define SH_S_ADDR(x) SN(SH_ADDR_SIZE, x)
#define SH_U_REG(x)  UN(SH_REG_SIZE, (x))
#define SH_S_REG(x)  SN(SH_REG_SIZE, (x))
#define SH_BIT(x)    UN(1, x)
#define SH_TRUE      SH_U_REG(1)
#define SH_FALSE     SH_U_REG(0)

#define sh_return_val_if_invalid_gpr(x, v) \
	if (!sh_valid_gpr(x)) { \
		RZ_LOG_ERROR("RzIL: SuperH: invalid register R%u\n", x); \
		return v; \
	}

#define sh_il_get_pure_param(x) \
	sh_il_get_param(op->param[x], op->scaling).pure

#define sh_il_set_pure_param(x, val) \
	sh_il_set_param(op->param[x], val, op->scaling)

#define sh_il_get_effective_addr_param(x) \
	sh_il_get_effective_addr_pc(op->param[x], op->scaling, pc)

/* Utilities */

static bool sh_valid_gpr(ut16 reg) {
	return reg < SH_GPR_COUNT;
}

static bool sh_banked_reg(ut16 reg) {
	return reg < SH_BANKED_REG_COUNT;
}

/**
 * Registers available as global variables in the IL
 */
static const char *sh_global_registers[] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", ///< bank 0 registers (user mode)
	"r0b", "r1b", "r2b", "r3b", "r4b", "r5b", "r6b", "r7b", ///< bank 1 registers (privileged mode)
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "sr",
	SH_SR_T, SH_SR_S, SH_SR_I, SH_SR_I, SH_SR_Q, SH_SR_M, SH_SR_F, SH_SR_B, SH_SR_R, SH_SR_D, ///< status register bits
	"gbr", "ssr", "spc", "sgr", "dbr", "vbr", "mach", "macl", "pr"
};

static const char *sh_get_banked_reg(ut16 reg, ut8 bank) {
	if (!sh_banked_reg(reg) || bank > 1) {
		return NULL;
	}
	return sh_global_registers[reg + bank * SH_BANKED_REG_COUNT];
}

/**
 * \brief We need this because sometimes we would want an `RzILOpBitvector` back
 * when we ask for a status reg bit, so this returns us an RzILOpBitvector instead
 * of the `RzILOpBool` returned when using `VARG`
 */
static RzILOpPure *sh_il_get_status_reg_bit(const char *bit) {
	return ITE(VARG(bit), SH_TRUE, SH_FALSE);
}

static RzILOpPure *sh_il_get_status_reg() {
	RzILOpPure *val = SH_U_REG(0);
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_D)), val);
	val = SHIFTL0(val, SH_U_REG(1));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_R)), val);
	val = SHIFTL0(val, SH_U_REG(1));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_B)), val);
	val = SHIFTL0(val, SH_U_REG(13));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_F)), val);
	val = SHIFTL0(val, SH_U_REG(6));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_M)), val);
	val = SHIFTL0(val, SH_U_REG(1));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_Q)), val);
	val = SHIFTL0(val, SH_U_REG(4));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_I)), val);
	val = SHIFTL0(val, SH_U_REG(3));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_S)), val);
	val = SHIFTL0(val, SH_U_REG(1));
	val = LOGOR(UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_T)), val);

	return val;
}

static RzILOpEffect *sh_il_set_status_reg(RzILOpPure *val) {
	RzILOpEffect *eff = SETG(SH_SR_T, NON_ZERO(LOGAND(SH_U_REG(0x1), val)));
	val = SHIFTR0(DUP(val), SH_U_REG(1));
	eff = SEQ2(eff, SETG(SH_SR_S, NON_ZERO(LOGAND(SH_U_REG(0x1), val))));
	val = SHIFTR0(DUP(val), SH_U_REG(3));
	eff = SEQ2(eff, SETG(SH_SR_I, NON_ZERO(LOGAND(SH_U_REG(0xf), val))));
	val = SHIFTR0(DUP(val), SH_U_REG(4));
	eff = SEQ2(eff, SETG(SH_SR_Q, NON_ZERO(LOGAND(SH_U_REG(0x1), val))));
	val = SHIFTR0(DUP(val), SH_U_REG(1));
	eff = SEQ2(eff, SETG(SH_SR_M, NON_ZERO(LOGAND(SH_U_REG(0x1), val))));
	val = SHIFTR0(DUP(val), SH_U_REG(6));
	eff = SEQ2(eff, SETG(SH_SR_F, NON_ZERO(LOGAND(SH_U_REG(0x1), val))));
	val = SHIFTR0(DUP(val), SH_U_REG(13));
	eff = SEQ2(eff, SETG(SH_SR_B, NON_ZERO(LOGAND(SH_U_REG(0x1), val))));
	val = SHIFTR0(DUP(val), SH_U_REG(1));
	eff = SEQ2(eff, SETG(SH_SR_R, NON_ZERO(LOGAND(SH_U_REG(0x1), val))));
	val = SHIFTR0(DUP(val), SH_U_REG(1));
	eff = SEQ2(eff, SETG(SH_SR_D, NON_ZERO(LOGAND(SH_U_REG(0x1), val))));

	return eff;
}

static RzILOpPure *sh_il_get_reg(ut16 reg) {
	sh_return_val_if_invalid_gpr(reg, NULL);
	if (!sh_banked_reg(reg)) {
		if (reg == SH_REG_IND_SR) {
			return sh_il_get_status_reg();
		}
		return VARG(sh_registers[reg]);
	}

	// check if both SR.MD = 1 and SR.RB = 1
	RzILOpPure *condition = AND(VARG(SH_SR_D), VARG(SH_SR_R));
	return ITE(condition, VARG(sh_get_banked_reg(reg, 1)), VARG(sh_get_banked_reg(reg, 0)));
}

static RzILOpEffect *sh_il_set_reg(ut16 reg, RZ_OWN RzILOpPure *val) {
	sh_return_val_if_invalid_gpr(reg, NULL);
	if (!sh_banked_reg(reg)) {
		if (reg == SH_REG_IND_SR) {
			return sh_il_set_status_reg(val);
		}
		return SETG(sh_registers[reg], val);
	}

	RzILOpPure *condition = AND(VARG(SH_SR_D), VARG(SH_SR_R));
	return BRANCH(condition, SETG(sh_get_banked_reg(reg, 1), val), SETG(sh_get_banked_reg(reg, 0), DUP(val)));
}

typedef struct sh_param_helper_t {
	RzILOpEffect *pre;
	RzILOpPure *pure;
	RzILOpEffect *post;
} SHParamHelper;

static RzILOpPure *sh_il_get_effective_addr_pc(SHParam param, SHScaling scaling, ut64 pc) {
	switch (param.mode) {
	case SH_REG_INDIRECT:
	case SH_REG_INDIRECT_I:
	case SH_REG_INDIRECT_D:
		return sh_il_get_reg(param.param[0]);
	case SH_REG_INDIRECT_DISP:
		return ADD(sh_il_get_reg(param.param[0]), MUL(SH_U_ADDR(param.param[1]), SH_U_ADDR(sh_scaling_size[scaling])));
	case SH_REG_INDIRECT_INDEXED:
		return ADD(sh_il_get_reg(SH_REG_IND_R0), sh_il_get_reg(param.param[0]));
	case SH_GBR_INDIRECT_DISP:
		return ADD(VARG("gbr"), MUL(SH_U_ADDR(param.param[0]), SH_U_ADDR(sh_scaling_size[scaling])));
	case SH_GBR_INDIRECT_INDEXED:
		return ADD(VARG("gbr"), sh_il_get_reg(SH_REG_IND_R0));
	case SH_PC_RELATIVE_DISP: {
		RzILOpBitVector *pcbv = SH_U_ADDR(pc);
		// mask lower 2 bits if long word
		if (scaling == SH_SCALING_L) {
			pcbv = LOGAND(pcbv, SH_U_ADDR(0xfffffffc));
		}
		pcbv = ADD(pcbv, SH_U_ADDR(4));
		return ADD(pcbv, MUL(SH_U_ADDR(param.param[0]), SH_U_ADDR(sh_scaling_size[scaling])));
	}
	case SH_PC_RELATIVE8: {
		// sign-extended for 8 bits and shifted left by 1 (i.e. multiplied by 2)
		RzILOpBitVector *relative = SIGNED(SH_ADDR_SIZE, SHIFTL0(SN(8, param.param[0]), U8(1)));
		return ADD(ADD(SH_U_ADDR(pc), SH_U_ADDR(4)), relative);
	}
	case SH_PC_RELATIVE12: {
		// sign-extended for 12 bits and shifted left by 1 (i.e. multiplied by 2)
		RzILOpBitVector *relative = SIGNED(SH_ADDR_SIZE, SHIFTL0(SN(12, param.param[0]), U8(1)));
		return ADD(ADD(SH_U_ADDR(pc), SH_U_ADDR(4)), relative);
	}
	case SH_PC_RELATIVE_REG:
		return ADD(ADD(SH_U_ADDR(pc), SH_U_ADDR(4)), sh_il_get_reg(param.param[0]));
	default:
		RZ_LOG_WARN("RzIL: SuperH: No effective address for this mode: %u\n", param.mode);
	}

	return NULL;
}

#define sh_il_get_effective_addr(x, y) sh_il_get_effective_addr_pc(x, y, pc)

static SHParamHelper sh_il_get_param_pc(SHParam param, SHScaling scaling, ut64 pc) {
	SHParamHelper ret = {
		.pre = NULL,
		.pure = NULL,
		.post = NULL
	};
	switch (param.mode) {
	case SH_REG_DIRECT:
		if (scaling == SH_SCALING_INVALID || scaling == SH_SCALING_L) {
			ret.pure = sh_il_get_reg(param.param[0]);
		} else {
			ret.pure = UNSIGNED(BITS_PER_BYTE * sh_scaling_size[scaling], sh_il_get_reg(param.param[0]));
		}
		break;
	case SH_REG_INDIRECT_I:
		ret.post = sh_il_set_reg(param.param[0], ADD(sh_il_get_reg(param.param[0]), SH_U_ADDR(sh_scaling_size[scaling])));
		goto set_pure;
	case SH_REG_INDIRECT_D:
		ret.pre = sh_il_set_reg(param.param[0], SUB(sh_il_get_reg(param.param[0]), SH_U_ADDR(sh_scaling_size[scaling])));
		goto set_pure;
	case SH_REG_INDIRECT:
	case SH_REG_INDIRECT_DISP:
	case SH_REG_INDIRECT_INDEXED:
	case SH_GBR_INDIRECT_DISP:
	case SH_GBR_INDIRECT_INDEXED:
	case SH_PC_RELATIVE_DISP:
	case SH_PC_RELATIVE8:
	case SH_PC_RELATIVE12:
	case SH_PC_RELATIVE_REG:
	set_pure:
		ret.pure = LOADW(BITS_PER_BYTE * sh_scaling_size[scaling], sh_il_get_effective_addr(param, scaling));
		break;
	case SH_IMM_U:
		ret.pure = SH_U_REG(param.param[0]);
		break;
	case SH_IMM_S:
		ret.pure = SH_S_REG(param.param[0]);
		break;
	default:
		RZ_LOG_ERROR("RzIL: SuperH: Invalid addressing mode\n");
	}

	return ret;
}

#define sh_il_get_param(x, y) sh_il_get_param_pc(x, y, pc)

static RzILOpEffect *sh_apply_effects(RzILOpEffect *target, RzILOpEffect *pre, RzILOpEffect *post) {
	if (!target) {
		if (pre) {
			target = pre;
			goto append;
		} else if (post) {
			return post;
		}
		return NULL;
	}

	if (pre) {
		target = SEQ2(pre, target);
	}
append:
	if (post) {
		target = SEQ2(target, post);
	}

	return target;
}

static RzILOpEffect *sh_il_set_param_pc(SHParam param, RZ_OWN RzILOpPure *val, SHScaling scaling, ut64 pc) {
	RzILOpEffect *ret = NULL, *pre = NULL, *post = NULL;
	switch (param.mode) {
	case SH_REG_DIRECT:
		if (scaling == SH_SCALING_INVALID || scaling == SH_SCALING_L) {
			ret = sh_il_set_reg(param.param[0], val);
		} else {
			ret = sh_il_set_reg(param.param[0], SIGNED(SH_REG_SIZE, val));
		}
		break;
	case SH_REG_INDIRECT:
	case SH_REG_INDIRECT_I:
	case SH_REG_INDIRECT_D:
	case SH_REG_INDIRECT_DISP:
	case SH_REG_INDIRECT_INDEXED:
	case SH_GBR_INDIRECT_DISP:
	case SH_GBR_INDIRECT_INDEXED:
	case SH_PC_RELATIVE_DISP:
	case SH_PC_RELATIVE8:
	case SH_PC_RELATIVE12:
	case SH_PC_RELATIVE_REG:
		break;
	case SH_IMM_U:
	case SH_IMM_S:
	default:
		RZ_LOG_ERROR("RzIL: SuperH: Cannot set value for addressing mode: %u\n", param.mode);
		return NULL;
	}

	if (!ret) {
		SHParamHelper ret_h = sh_il_get_param(param, scaling);
		RZ_FREE(ret_h.pure);
		RzILOpPure *eff_addr = sh_il_get_effective_addr(param, scaling);
		ret = STOREW(eff_addr, val);
		pre = ret_h.pre;
		post = ret_h.post;
	}

	return sh_apply_effects(ret, pre, post);
}

#define sh_il_set_param(x, y, z) sh_il_set_param_pc(x, y, z, pc)

static RzILOpBool *sh_il_is_add_carry(RZ_OWN RzILOpPure *res, RZ_OWN RzILOpPure *x, RZ_OWN RzILOpPure *y) {
	// res = x + y
	RzILOpBool *xmsb = MSB(x);
	RzILOpBool *ymsb = MSB(y);
	RzILOpBool *resmsb = MSB(res);

	// x & y
	RzILOpBool *xy = AND(xmsb, ymsb);
	RzILOpBool *nres = INV(resmsb);

	// !res & y
	RzILOpBool *ry = AND(nres, DUP(ymsb));
	// x & !res
	RzILOpBool *xr = AND(DUP(xmsb), nres);

	// bit = xy | ry | xr
	RzILOpBool * or = OR(xy, ry);
	or = OR(or, xr);

	return or ;
}

static RzILOpBool *sh_il_is_sub_borrow(RZ_OWN RzILOpPure *res, RZ_OWN RzILOpPure *x, RZ_OWN RzILOpPure *y) {
	// res = x - y
	RzILOpBool *xmsb = MSB(x);
	RzILOpBool *ymsb = MSB(y);
	RzILOpBool *resmsb = MSB(res);

	// !x & y
	RzILOpBool *nx = INV(xmsb);
	RzILOpBool *nxy = AND(nx, ymsb);

	// y & res
	RzILOpBool *rny = AND(DUP(ymsb), resmsb);
	// res & !x
	RzILOpBool *rnx = AND(DUP(resmsb), nx);

	// bit = nxy | rny | rnx
	RzILOpBool * or = OR(nxy, rny);
	or = OR(or, rnx);

	return or ;
}

static RzILOpBool *sh_il_is_add_overflow(RZ_OWN RzILOpPure *res, RZ_OWN RzILOpPure *x, RZ_OWN RzILOpPure *y) {
	// res = x + y
	RzILOpBool *xmsb = MSB(x);
	RzILOpBool *ymsb = MSB(y);
	RzILOpBool *resmsb = MSB(res);

	// !res & x & y
	RzILOpBool *nrxy = AND(AND(INV(resmsb), xmsb), ymsb);
	// res & !x & !y
	RzILOpBool *rnxny = AND(AND(DUP(resmsb), INV(DUP(xmsb))), INV(DUP(ymsb)));
	// or = nrxy | rnxny
	RzILOpBool * or = OR(nrxy, rnxny);

	return or ;
}

static RzILOpBool *sh_il_is_sub_underflow(RZ_OWN RzILOpPure *res, RZ_OWN RzILOpPure *x, RZ_OWN RzILOpPure *y) {
	// res = x - y
	RzILOpBool *xmsb = MSB(x);
	RzILOpBool *ymsb = MSB(y);
	RzILOpBool *resmsb = MSB(res);

	// !res & x & !y
	RzILOpBool *nrxny = AND(AND(INV(resmsb), xmsb), INV(ymsb));
	// res & !x & y
	RzILOpBool *rnxy = AND(AND(DUP(resmsb), INV(DUP(xmsb))), DUP(ymsb));
	// or = nrxny | rnxy
	RzILOpBool * or = OR(nrxny, rnxy);

	return or ;
}

/* Instruction implementations */

/**
 * Unknown instruction
 */
static RzILOpEffect *sh_il_invalid(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return NULL;
}

/**
 * MOV family instructions
 */
static RzILOpEffect *sh_il_mov(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	SHParamHelper shp = sh_il_get_param(op->param[0], op->scaling);
	return sh_apply_effects(sh_il_set_pure_param(1, shp.pure), shp.pre, shp.post);
}

/**
 * MOVT	 Rn
 * T -> Rn
 * 0000nnnn00101001
 */
static RzILOpEffect *sh_il_movt(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(0, UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_T)));
}

/**
 * SWAP.B  Rm, Rn
 * Rm -> swap lower 2 bytes -> REG
 * 0110nnnnmmmm1000
 *
 * SWAP.W  Rm, Rn
 * Rm -> swap upper/lower words -> Rn
 * 0110nnnnmmmm1001
 */
static RzILOpEffect *sh_il_swap(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	if (op->scaling == SH_SCALING_B) {
		// swap lower two bytes
		RzILOpPure *lower_byte = AND(sh_il_get_pure_param(0), SH_U_REG(0xff));
		RzILOpPure *new_lower_byte = AND(SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(BITS_PER_BYTE)), SH_U_REG(0xff));
		RzILOpPure *new_upper_byte = SHIFTL0(lower_byte, SH_U_REG(BITS_PER_BYTE));
		RzILOpPure *upper_word = LOGAND(sh_il_get_pure_param(0), SH_U_REG(0xffff0000));
		return sh_il_set_pure_param(1, LOGOR(upper_word, LOGOR(new_upper_byte, new_lower_byte)));
	} else if (op->scaling == SH_SCALING_W) {
		// swap upper and lower words and store in dst
		RzILOpPure *high = SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(BITS_PER_BYTE * 2));
		RzILOpPure *low = SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(BITS_PER_BYTE * 2));
		return sh_il_set_pure_param(1, LOGOR(high, low));
	}

	return NULL;
}

/**
 * XTRCT  Rm, Rn
 * Rm:Rn middle 32 bits -> Rn
 * 0010nnnnmmmm1101
 */
static RzILOpEffect *sh_il_xtrct(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *high = SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(BITS_PER_BYTE * 2));
	RzILOpPure *low = SHIFTR0(sh_il_get_pure_param(1), SH_U_REG(BITS_PER_BYTE * 2));
	return sh_il_set_pure_param(1, LOGOR(high, low));
}

/**
 * ADD  Rm, Rn
 * Rn + Rm -> Rn
 * 0011nnnnmmmm1100
 *
 * ADD #imm, Rn
 * Rn + imm -> Rn
 * 0111nnnniiiiiiii
 */
static RzILOpEffect *sh_il_add(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, ADD(sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
}

/**
 * ADDC  Rm, Rn
 * Rn + Rm + T -> Rn
 * carry -> T
 * 0011nnnnmmmm1110
 */
static RzILOpEffect *sh_il_addc(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *sum = ADD(sh_il_get_pure_param(0), sh_il_get_pure_param(1));
	sum = ADD(sum, UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_T)));

	RzILOpEffect *ret = sh_il_set_pure_param(1, sum);
	RzILOpEffect *tbit = SETG(SH_SR_T, sh_il_is_add_carry(DUP(sum), sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
	return SEQ2(ret, tbit);
}

/**
 * ADDV  Rm, Rn
 * Rn + Rm -> Rn
 * overflow -> T
 * 0011nnnnmmmm1111
 */
static RzILOpEffect *sh_il_addv(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *sum = ADD(sh_il_get_pure_param(0), sh_il_get_pure_param(1));

	RzILOpEffect *ret = sh_il_set_pure_param(1, sum);
	RzILOpEffect *tbit = SETG(SH_SR_T, sh_il_is_add_overflow(DUP(sum), sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
	return SEQ2(ret, tbit);
}

/**
 * CMP/EQ  #imm, R0
 * When R0 = imm, 1 -> T ; Otherwise, 0 -> T
 * 10001000iiiiiiii
 *
 * CMP/EQ  Rm, Rn
 * When Rn = Rm, 1 -> T ; Otherwise, 0 -> T
 * 0011nnnnmmmm0000
 */
static RzILOpEffect *sh_il_cmp_eq(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, EQ(sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
}

/**
 * CMP/HS  Rm, Rn
 * When Rn >= Rm (unsigned), 1 -> T ; Otherwise, 0 -> T
 * 0011nnnnmmmm0010
 */
static RzILOpEffect *sh_il_cmp_hs(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, UGE(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
}

/**
 * CMP/GE  Rm, Rn
 * When Rn >= Rm (signed), 1 -> T ; Otherwise, 0 -> T
 * 0011nnnnmmmm0011
 */
static RzILOpEffect *sh_il_cmp_ge(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, SGE(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
}

/**
 * CMP/HI  Rm, Rn
 * When Rn > Rm (unsigned), 1 -> T ; Otherwise, 0 -> T
 * 0011nnnnmmmm0110
 */
static RzILOpEffect *sh_il_cmp_hi(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, UGT(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
}

/**
 * CMP/GT  Rm, Rn
 * When Rn > Rm (signed), 1 -> T ; Otherwise, 0 -> T
 * 0011nnnnmmmm0111
 */
static RzILOpEffect *sh_il_cmp_gt(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, SGT(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
}

/**
 * CMP/PZ  Rn
 * When Rn >= 0, 1 -> T ; Otherwise, 0 -> T
 * 0100nnnn00010001
 */
static RzILOpEffect *sh_il_cmp_pz(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, SGE(sh_il_get_pure_param(0), SH_S_REG(0)));
}

/**
 * CMP/PL  Rn
 * When Rn > 0, 1 -> T ; Otherwise, 0 -> T
 * 0100nnnn00010101
 */
static RzILOpEffect *sh_il_cmp_pl(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, SGT(sh_il_get_pure_param(0), SH_S_REG(0)));
}

/**
 * CMP/STR  Rm, Rn
 * When any bytes are equal, 1 -> T ; Otherwise, 0 -> T
 * 0010nnnnmmmm1100
 */
static RzILOpEffect *sh_il_cmp_str(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure * xor = XOR(sh_il_get_pure_param(0), sh_il_get_pure_param(1));

	RzILOpPure *eq = EQ(LOGAND(xor, SH_U_REG(0xff)), SH_U_REG(0x0));
	xor = SHIFTR0(DUP(xor), SH_U_REG(BITS_PER_BYTE));
	eq = OR(eq, EQ(LOGAND(xor, SH_U_REG(0xff)), SH_U_REG(0x0)));
	xor = SHIFTR0(DUP(xor), SH_U_REG(BITS_PER_BYTE));
	eq = OR(eq, EQ(LOGAND(xor, SH_U_REG(0xff)), SH_U_REG(0x0)));
	xor = SHIFTR0(DUP(xor), SH_U_REG(BITS_PER_BYTE));
	eq = OR(eq, EQ(LOGAND(xor, SH_U_REG(0xff)), SH_U_REG(0x0)));

	return SETG(SH_SR_T, eq);
}

/**
 * DIV1  Rm, Rn
 * 1-step division (Rn ÷ Rm) ; Calculation result -> T
 * 0011nnnnmmmm0100
 *
 * Implementation details at page 162 (of 512) in https://www.renesas.com/eu/en/document/mah/sh-1sh-2sh-dsp-software-manual?language=en
 */
static RzILOpEffect *sh_il_div1(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *old_q = SETL("old_q", sh_il_get_status_reg_bit(SH_SR_Q));
	RzILOpEffect *q = SETG(SH_SR_Q, MSB(sh_il_get_pure_param(1)));
	RzILOpEffect *shl = sh_il_set_pure_param(1, SHIFTL0(sh_il_get_pure_param(1), SH_U_REG(1)));
	RzILOpEffect *ort = sh_il_set_pure_param(1, OR(sh_il_get_pure_param(1), UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_T))));
	RzILOpEffect *init = SEQ4(old_q, q, shl, ort);

	RzILOpEffect *tmp0 = SETL("tmp0", sh_il_get_pure_param(1));
	RzILOpEffect *sub = sh_il_set_pure_param(1, SUB(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
	RzILOpEffect *tmp1 = SETL("tmp1", UGT(sh_il_get_pure_param(1), VARL("tmp0")));
	RzILOpEffect *q_bit = BRANCH(sh_il_get_status_reg_bit(SH_SR_Q), SETG(SH_SR_Q, IS_ZERO(VARL("tmp1"))), SETG(SH_SR_Q, VARL("tmp1")));
	RzILOpEffect *q0m0 = SEQ4(tmp0, sub, tmp1, q_bit);

	tmp0 = SETL("tmp0", sh_il_get_pure_param(1));
	RzILOpEffect *add = sh_il_set_pure_param(1, ADD(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
	tmp1 = SETL("tmp1", ULT(sh_il_get_pure_param(1), VARL("tmp0")));
	q_bit = BRANCH(sh_il_get_status_reg_bit(SH_SR_Q), SETG(SH_SR_Q, VARL("tmp1")), SETG(SH_SR_Q, IS_ZERO(VARL("tmp1"))));
	RzILOpEffect *q0m1 = SEQ4(tmp0, add, tmp1, q_bit);

	tmp0 = SETL("tmp0", sh_il_get_pure_param(1));
	add = sh_il_set_pure_param(1, ADD(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
	tmp1 = SETL("tmp1", ULT(sh_il_get_pure_param(1), VARL("tmp0")));
	q_bit = BRANCH(sh_il_get_status_reg_bit(SH_SR_Q), SETG(SH_SR_Q, IS_ZERO(VARL("tmp1"))), SETG(SH_SR_Q, VARL("tmp1")));
	RzILOpEffect *q1m0 = SEQ4(tmp0, add, tmp1, q_bit);

	tmp0 = SETL("tmp0", sh_il_get_pure_param(1));
	sub = sh_il_set_pure_param(1, SUB(sh_il_get_pure_param(1), sh_il_get_pure_param(0)));
	tmp1 = SETL("tmp1", UGT(sh_il_get_pure_param(1), VARL("tmp0")));
	q_bit = BRANCH(sh_il_get_status_reg_bit(SH_SR_Q), SETG(SH_SR_Q, VARL("tmp1")), SETG(SH_SR_Q, IS_ZERO(VARL("tmp1"))));
	RzILOpEffect *q1m1 = SEQ4(tmp0, sub, tmp1, q_bit);

	RzILOpEffect *q0 = BRANCH(sh_il_get_status_reg_bit(SH_SR_M), q0m1, q0m0);
	RzILOpEffect *q1 = BRANCH(sh_il_get_status_reg_bit(SH_SR_M), q1m1, q1m0);
	RzILOpEffect *q_switch = BRANCH(VARL("old_q"), q1, q0);

	return SEQ3(init, q_switch, SETG(SH_SR_T, EQ(sh_il_get_status_reg_bit(SH_SR_Q), sh_il_get_status_reg_bit(SH_SR_M))));
}

/**
 * DIV0S  Rm, Rn
 * MSB of Rn -> Q ; MSB of Rm -> M, M^Q -> T
 * 0010nnnnmmmm0111
 */
static RzILOpEffect *sh_il_div0s(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *setm = SETG(SH_SR_M, MSB(sh_il_get_pure_param(0)));
	RzILOpEffect *setq = SETG(SH_SR_Q, MSB(sh_il_get_pure_param(1)));
	RzILOpEffect *sett = SETG(SH_SR_T, XOR(MSB(sh_il_get_pure_param(0)), MSB(sh_il_get_pure_param(1))));

	return SEQ3(setm, setq, sett);
}

/**
 * DIV0U  Rm, Rn
 * 0 -> M/Q/T
 * 0000000000011001
 */
static RzILOpEffect *sh_il_div0u(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SEQ3(SETG(SH_SR_M, SH_BIT(0)), SETG(SH_SR_Q, SH_BIT(0)), SETG(SH_SR_T, SH_BIT(0)));
}

/**
 * DMULS.L  Rm, Rn
 * Signed, Rn * Rm -> MAC ; 32 * 32 -> 64 bits
 * 0011nnnnmmmm1101
 */
static RzILOpEffect *sh_il_dmuls(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *eff = SETL("res_wide", MUL(SIGNED(2 * SH_REG_SIZE, sh_il_get_pure_param(0)), SIGNED(2 * SH_REG_SIZE, sh_il_get_pure_param(1))));
	RzILOpPure *lower_bits = UNSIGNED(SH_REG_SIZE, LOGAND(VARL("res_wide"), UN(2 * SH_REG_SIZE, 0xffffffff)));
	RzILOpPure *higher_bits = UNSIGNED(SH_REG_SIZE, SHIFTR0(VARL("res_wide"), SH_U_REG(SH_REG_SIZE)));
	return SEQ3(eff, SETG("macl", lower_bits), SETG("mach", higher_bits));
}

/**
 * DMULU.L  Rm, Rn
 * Unsigned, Rn * Rm -> MAC ; 32 * 32 -> 64 bits
 * 0011nnnnmmmm0101
 */
static RzILOpEffect *sh_il_dmulu(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *eff = SETL("res_wide", MUL(UNSIGNED(2 * SH_REG_SIZE, sh_il_get_pure_param(0)), UNSIGNED(2 * SH_REG_SIZE, sh_il_get_pure_param(1))));
	RzILOpPure *lower_bits = UNSIGNED(SH_REG_SIZE, LOGAND(VARL("res_wide"), UN(2 * SH_REG_SIZE, 0xffffffff)));
	RzILOpPure *higher_bits = UNSIGNED(SH_REG_SIZE, SHIFTR0(VARL("res_wide"), SH_U_REG(SH_REG_SIZE)));
	return SEQ3(eff, SETG("macl", lower_bits), SETG("mach", higher_bits));
}

/**
 * DT  Rn
 * Rn - 1 -> Rn ; When Rn = 0, 1 -> T ; Otherwise 0 -> T
 * 0100nnnn00010000
 */
static RzILOpEffect *sh_il_dt(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SEQ2(sh_il_set_pure_param(0, SUB(sh_il_get_pure_param(0), SH_U_REG(1))), SETG(SH_SR_T, NON_ZERO(sh_il_get_pure_param(0))));
}

/**
 * EXTS.B  Rm, Rn
 * Rm sign-extended from byte -> Rn
 * 0110nnnnmmmm1110
 *
 * EXTS.W  Rm, Rn
 * Rm sign-extended from word -> Rn
 * 0110nnnnmmmm1111
 */
static RzILOpEffect *sh_il_exts(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *eff = NULL;
	if (op->scaling == SH_SCALING_B) {
		RzILOpPure *byte = LOGAND(sh_il_get_pure_param(0), SH_U_REG(0xff));
		RzILOpBool *msb = MSB(byte);
		eff = BRANCH(msb, sh_il_set_pure_param(1, LOGOR(DUP(byte), SH_U_REG(0xffffff00))), sh_il_set_pure_param(1, DUP(byte)));
	} else if (op->scaling == SH_SCALING_W) {
		RzILOpPure *word = LOGAND(sh_il_get_pure_param(0), SH_U_REG(0xffff));
		RzILOpBool *msb = MSB(word);
		eff = BRANCH(msb, sh_il_set_pure_param(1, LOGOR(DUP(word), SH_U_REG(0xffff0000))), sh_il_set_pure_param(1, DUP(word)));
	}

	return eff;
}

/**
 * EXTU.B  Rm, Rn
 * Rm zero-extended from byte -> Rn
 * 0110nnnnmmmm1100
 *
 * EXTU.W  Rm, Rn
 * Rm zero-extended from word -> Rn
 * 0110nnnnmmmm1101
 */
static RzILOpEffect *sh_il_extu(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *eff = NULL;
	if (op->scaling == SH_SCALING_B) {
		eff = sh_il_set_pure_param(1, LOGAND(sh_il_get_pure_param(0), SH_U_REG(0xff)));
	} else if (op->scaling == SH_SCALING_W) {
		eff = sh_il_set_pure_param(1, LOGAND(sh_il_get_pure_param(0), SH_U_REG(0xffff)));
	}

	return eff;
}

/**
 * MAC.L  @Rm+, @Rn+
 * Rn * Rm + MAC -> MAC (Signed) (32 * 32 + 64 -> 64 bits)
 * Rn + 4 -> Rn ; Rm + 4 -> Rm
 * 0000nnnnmmmm1111
 *
 * When S bit is enabled, the MAC addition is a saturation operation of 48 bits
 * So only the lower 48 bits of result and MAC are considered
 *
 * MAC.W  @Rm+, @Rn+
 * Rn * Rm + MAC -> MAC (Signed) (16 * 16 + 64 -> 64 bits)
 * Rn + 2 -> Rn ; Rm + 2 -> Rm
 * 0000nnnnmmmm1111
 *
 * When S bit is enabled, the MAC addition is a saturation operation of 32 bits
 * So only the lower 32 bits of result and MAC are considered (which is basically MACL register)
 */
static RzILOpEffect *sh_il_mac(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	SHParamHelper shp_rm = sh_il_get_param(op->param[0], op->scaling);
	SHParamHelper shp_rn = sh_il_get_param(op->param[1], op->scaling);
	RzILOpEffect *eff = NULL;

	if (op->scaling == SH_SCALING_L) {
		RzILOpEffect *mac = SETL("mac", LOGOR(SHIFTL0((UNSIGNED(2 * SH_REG_SIZE, VARG("mach"))), SH_U_REG(SH_REG_SIZE)), UNSIGNED(2 * SH_REG_SIZE, VARG("macl"))));
		RzILOpPure *mul = MUL(SIGNED(2 * SH_REG_SIZE, shp_rm.pure), SIGNED(2 * SH_REG_SIZE, shp_rn.pure));
		RzILOpPure *add = ADD(mul, VARL("mac"));
		RzILOpPure *low = UNSIGNED(48, LOGAND(add, UN(2 * SH_REG_SIZE, 0xffffffffffff)));
		RzILOpPure *sat = SIGNED(2 * SH_REG_SIZE, low);

		eff = SEQ2(mac, BRANCH(sh_il_get_status_reg_bit(SH_SR_S), SETL("mac", sat), SETG("mac", DUP(add))));
		RzILOpPure *lower_bits = UNSIGNED(SH_REG_SIZE, LOGAND(VARL("mac"), UN(2 * SH_REG_SIZE, 0xffffffff)));
		RzILOpPure *higher_bits = UNSIGNED(SH_REG_SIZE, SHIFTR0(VARL("mac"), SH_U_REG(SH_REG_SIZE)));
		eff = SEQ3(eff, SETG("macl", lower_bits), SETG("mach", higher_bits));
	} else if (op->scaling == SH_SCALING_W) {
		RzILOpEffect *mac = SETL("mac", LOGOR(SHIFTL0((UNSIGNED(2 * SH_REG_SIZE, VARG("mach"))), SH_U_REG(SH_REG_SIZE)), UNSIGNED(2 * SH_REG_SIZE, VARG("macl"))));
		RzILOpPure *mul = UNSIGNED(2 * SH_REG_SIZE, MUL(SIGNED(SH_REG_SIZE, shp_rm.pure), SIGNED(SH_REG_SIZE, shp_rn.pure)));
		RzILOpPure *add = ADD(mul, VARG("mac"));
		RzILOpPure *sat_add = ADD(UNSIGNED(SH_REG_SIZE, DUP(mul)), VARG("macl"));
		RzILOpPure *lower_bits = UNSIGNED(SH_REG_SIZE, LOGAND(add, UN(2 * SH_REG_SIZE, 0xffffffff)));
		RzILOpPure *higher_bits = UNSIGNED(SH_REG_SIZE, SHIFTR0(DUP(add), SH_U_REG(SH_REG_SIZE)));

		eff = SEQ2(mac, BRANCH(sh_il_get_status_reg_bit(SH_SR_S), SETG("macl", sat_add), SEQ2(SETG("macl", lower_bits), SETG("mach", higher_bits))));
	}

	eff = SEQ3(eff, shp_rn.post, shp_rm.post);
	return eff;
}

/**
 * MUL.L  Rm, Rn
 * Rn * Rm -> MACL (32 * 32 -> 32 bits)
 * 0000nnnnmmmm0111
 */
static RzILOpEffect *sh_il_mul(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG("macl", MUL(sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
}

/**
 * MULS.W  Rm, Rn
 * Rn * Rm -> MACL (Signed) (16 * 16 -> 32 bits)
 * 0010nnnnmmmm1111
 */
static RzILOpEffect *sh_il_muls(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *m = SIGNED(SH_REG_SIZE, SIGNED(16, sh_il_get_pure_param(0)));
	RzILOpPure *n = SIGNED(SH_REG_SIZE, SIGNED(16, sh_il_get_pure_param(1)));
	return SETG("macl", MUL(m, n));
}

/**
 * MULU.W  Rm, Rn
 * Rn * Rm -> MACL (Unsigned) (16 * 16 -> 32 bits)
 * 0010nnnnmmmm1110
 */
static RzILOpEffect *sh_il_mulu(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *m = UNSIGNED(SH_REG_SIZE, UNSIGNED(16, sh_il_get_pure_param(0)));
	RzILOpPure *n = UNSIGNED(SH_REG_SIZE, UNSIGNED(16, sh_il_get_pure_param(1)));
	return SETG("macl", MUL(m, n));
}

/**
 * NEG  Rm, Rn
 * 0 - Rm -> Rn
 * 0110nnnnmmmm1011
 */
static RzILOpEffect *sh_il_neg(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *sub = SUB(UNSIGNED(SH_REG_SIZE, 0), sh_il_get_pure_param(0));
	return sh_il_set_pure_param(1, sub);
}

/**
 * NEGC  Rm, Rn
 * 0 - Rm - T -> Rn ; borrow -> T
 * 0110nnnnmmmm1010
 */
static RzILOpEffect *sh_il_negc(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *sub = SUB(UNSIGNED(SH_REG_SIZE, 0), sh_il_get_pure_param(0));
	sub = SUB(sub, UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_T)));
	return SEQ2(sh_il_set_pure_param(1, sub), SETG(SH_SR_T, sh_il_is_sub_borrow(sub, UNSIGNED(SH_REG_SIZE, 0), sh_il_get_pure_param(0))));
}

/**
 * SUB  Rm, Rn
 * Rn - Rm -> Rn
 * 0011nnnnmmmm1000
 */
static RzILOpEffect *sh_il_sub(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, SUB(sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
}

/**
 * SUBC  Rm, Rn
 * Rn - Rm - T -> Rn ; borrow -> T
 * 0011nnnnmmmm1010
 */
static RzILOpEffect *sh_il_subc(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *dif = ADD(sh_il_get_pure_param(0), sh_il_get_pure_param(1));
	dif = SUB(dif, UNSIGNED(SH_REG_SIZE, sh_il_get_status_reg_bit(SH_SR_T)));

	RzILOpEffect *ret = sh_il_set_pure_param(1, dif);
	RzILOpEffect *tbit = SETG(SH_SR_T, sh_il_is_sub_borrow(DUP(dif), sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
	return SEQ2(ret, tbit);
}

/**
 * SUBV  Rm, Rn
 * Rn - Rm -> Rn ; underflow -> T
 * 0011nnnnmmmm1011
 */
static RzILOpEffect *sh_il_subv(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *dif = SUB(sh_il_get_pure_param(0), sh_il_get_pure_param(1));

	RzILOpEffect *ret = sh_il_set_pure_param(1, dif);
	RzILOpEffect *tbit = SETG(SH_SR_T, sh_il_is_sub_underflow(DUP(dif), sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
	return SEQ2(ret, tbit);
}

/**
 * AND  Rm, Rn
 * Rn & Rm -> Rn
 * 0010nnnnmmmm1001
 *
 * AND  #imm, R0
 * R0 & imm -> R0
 * 11001001iiiiiiii
 *
 * AND.B  #imm, @(R0, GBR)
 * (R0 + GBR) & imm -> (R0 + GBR)
 * 11001101iiiiiiii
 */
static RzILOpEffect *sh_il_and(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, LOGAND(sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
}

/**
 * NOT  Rm, Rn
 * ~Rm -> Rn
 * 0110nnnnmmmm0111
 */
static RzILOpEffect *sh_il_not(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, LOGNOT(sh_il_get_pure_param(0)));
}

/**
 * OR  Rm, Rn
 * Rn | Rm -> Rn
 * 0010nnnnmmmm1011
 *
 * OR  #imm, R0
 * R0 | imm -> R0
 * 11001011iiiiiiii
 *
 * OR.B  #imm, @(R0, GBR)
 * (R0 + GBR) | imm -> (R0 + GBR)
 * 11001111iiiiiiii
 */
static RzILOpEffect *sh_il_or(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, LOGOR(sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
}

/**
 * TAS.B  @Rn
 * If (Rn) = 0, 1 -> T ; Otherwise 0 -> T
 * 1 -> MSB of (Rn)
 * 0110nnnnmmmm0111
 */
static RzILOpEffect *sh_il_tas(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *mem = sh_il_get_pure_param(0);
	RzILOpEffect *tbit = SETG(SH_SR_T, IS_ZERO(mem));
	return SEQ2(tbit, sh_il_set_pure_param(0, LOGOR(DUP(mem), UN(8, 0x80))));
}

/**
 * TST  Rm, Rn
 * If Rn & Rm = 0, 1 -> T ; Otherwise 0 -> T
 * 0010nnnnmmmm1000
 *
 * TST  #imm, R0
 * If R0 & imm = 0, 1 -> T ; Otherwise 0 -> T
 * 11001000iiiiiiii
 *
 * TST.B  #imm, @(R0, GBR)
 * If (R0 + GBR) & imm = 0, 1 -> T ; Otherwise 0 -> T
 * 11001100iiiiiiii
 */
static RzILOpEffect *sh_il_tst(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, IS_ZERO(LOGAND(sh_il_get_pure_param(0), sh_il_get_pure_param(1))));
}

/**
 * XOR  Rm, Rn
 * Rn ^ Rm -> Rn
 * 0010nnnnmmmm1010
 *
 * XOR  #imm, R0
 * R0 ^ imm -> R0
 * 11001010iiiiiiii
 *
 * XOR.B  #imm, @(R0, GBR)
 * (R0 + GBR) ^ imm -> (R0 + GBR)
 * 11001110iiiiiiii
 */
static RzILOpEffect *sh_il_xor(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, LOGXOR(sh_il_get_pure_param(0), sh_il_get_pure_param(1)));
}

/**
 * ROTL  Rn
 * T <- Rn <- MSB
 * 0100nnnn00000100
 */
static RzILOpEffect *sh_il_rotl(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpBool *msb = MSB(sh_il_get_pure_param(0));
	RzILOpEffect *tbit = SETG(SH_SR_T, msb);
	RzILOpPure *shl = SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(1));
	RzILOpPure *lsb = ITE(DUP(msb), OR(shl, SH_U_REG(1)), AND(DUP(shl), SH_U_REG(0xfffffffe)));
	return SEQ2(tbit, sh_il_set_pure_param(0, lsb));
}

/**
 * ROTR  Rn
 * LSB -> Rn -> T
 * 0100nnnn00000101
 */
static RzILOpEffect *sh_il_rotr(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpBool *lsb = LSB(sh_il_get_pure_param(0));
	RzILOpEffect *tbit = SETG(SH_SR_T, lsb);
	RzILOpPure *shr = SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(1));
	RzILOpPure *msb = ITE(DUP(lsb), OR(shr, SH_U_REG(0x80000000)), AND(DUP(shr), SH_U_REG(0x7fffffff)));
	return SEQ2(tbit, sh_il_set_pure_param(0, msb));
}

/**
 * ROTCL  Rn
 * T <- Rn <- T
 * 0100nnnn00100100
 */
static RzILOpEffect *sh_il_rotcl(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *msb = SETL("msb", MSB(sh_il_get_pure_param(0)));
	RzILOpPure *shl = SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(1));
	RzILOpPure *lsb = ITE(sh_il_get_status_reg_bit(SH_SR_T), OR(shl, SH_U_REG(1)), AND(DUP(shl), SH_U_REG(0xfffffffe)));
	RzILOpEffect *tbit = SETG(SH_SR_T, VARL("msb"));
	return SEQ3(msb, sh_il_set_pure_param(0, lsb), tbit);
}

/**
 * ROTCR  Rn
 * T -> Rn -> T
 * 0100nnnn00100101
 */
static RzILOpEffect *sh_il_rotcr(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *lsb = SETL("lsb", LSB(sh_il_get_pure_param(0)));
	RzILOpPure *shr = SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(1));
	RzILOpPure *msb = ITE(sh_il_get_status_reg_bit(SH_SR_T), OR(shr, SH_U_REG(0x80000000)), AND(DUP(shr), SH_U_REG(0x7fffffff)));
	RzILOpEffect *tbit = SETG(SH_SR_T, VARL("lsb"));
	return SEQ3(lsb, sh_il_set_pure_param(0, msb), tbit);
}

/**
 * SHAD  Rm, Rn
 * If Rn >= 0, Rn << Rm -> Rn
 * If Rn < 0, Rn >> Rm -> [MSB -> Rn]
 * MSB -> Rn
 * 0100nnnnmmmm1100
 */
static RzILOpEffect *sh_il_shad(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *op1 = SETL("op1", SIGNED(32, sh_il_get_pure_param(0)));
	RzILOpEffect *op2 = SETL("op2", SIGNED(32, sh_il_get_pure_param(1)));
	RzILOpPure *shift_amount = UNSIGNED(5, VARL("op1"));

	RzILOpPure *shl = SHIFTL0(VARL("op2"), shift_amount);
	RzILOpPure *shr = SHIFTRA(VARL("op2"), SUB(UN(5, 32), DUP(shift_amount)));

	return SEQ3(op1, op2, BRANCH(SGE(VARL("op1"), SN(32, 0)), sh_il_set_pure_param(1, shl), sh_il_set_pure_param(1, shr)));
}

/**
 * SHAL  Rn
 * T <- Rn <- 0
 * 0100nnnn00100000
 */
static RzILOpEffect *sh_il_shal(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *msb = MSB(sh_il_get_pure_param(0));
	RzILOpPure *shl = SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(1));
	return SEQ2(SETG(SH_SR_T, msb), sh_il_set_pure_param(0, shl));
}

/**
 * SHAR  Rn
 * MSB -> Rn -> T
 * 0100nnnn00100001
 */
static RzILOpEffect *sh_il_shar(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *lsb = LSB(sh_il_get_pure_param(0));
	RzILOpPure *shl = SHIFTRA(sh_il_get_pure_param(0), SH_U_REG(1));
	return SEQ2(SETG(SH_SR_T, lsb), sh_il_set_pure_param(0, shl));
}

/**
 * SHLD  Rm, Rn
 * If Rn >= 0, Rn << Rm -> Rn
 * If Rn < 0, Rn >> Rm -> [0 -> Rn]
 * MSB -> Rn
 * 0100nnnnmmmm1101
 */
static RzILOpEffect *sh_il_shld(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpEffect *op1 = SETL("op1", SIGNED(32, sh_il_get_pure_param(0)));
	RzILOpEffect *op2 = SETL("op2", UNSIGNED(32, sh_il_get_pure_param(1)));
	RzILOpPure *shift_amount = UNSIGNED(5, VARL("op1"));

	RzILOpPure *shl = SHIFTL0(VARL("op2"), shift_amount);
	RzILOpPure *shr = SHIFTR0(VARL("op2"), SUB(UN(5, 32), DUP(shift_amount)));

	return SEQ3(op1, op2, BRANCH(SGE(VARL("op1"), SN(32, 0)), sh_il_set_pure_param(1, shl), sh_il_set_pure_param(1, shr)));
}

/**
 * SHLL  Rn
 * T <- Rn <- 0
 * 0100nnnn00000000
 */
static RzILOpEffect *sh_il_shll(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *msb = MSB(sh_il_get_pure_param(0));
	RzILOpPure *shl = SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(1));
	return SEQ2(SETG(SH_SR_T, msb), sh_il_set_pure_param(0, shl));
}

/**
 * SHLR  Rn
 * 0 -> Rn -> T
 * 0100nnnn00000001
 */
static RzILOpEffect *sh_il_shlr(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *lsb = LSB(sh_il_get_pure_param(0));
	RzILOpPure *shr = SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(1));
	return SEQ2(SETG(SH_SR_T, lsb), sh_il_set_pure_param(0, shr));
}

/**
 * SHLL2  Rn
 * Rn << 2 -> Rn
 * 0100nnnn00001000
 */
static RzILOpEffect *sh_il_shll2(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(0, SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(2)));
}

/**
 * SHLR2  Rn
 * Rn >> 2 -> Rn
 * 0100nnnn00001001
 */
static RzILOpEffect *sh_il_shlr2(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(0, SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(2)));
}

/**
 * SHLL8  Rn
 * Rn << 8 -> Rn
 * 0100nnnn00011000
 */
static RzILOpEffect *sh_il_shll8(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(0, SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(8)));
}

/**
 * SHLR8  Rn
 * Rn >> 8 -> Rn
 * 0100nnnn00011001
 */
static RzILOpEffect *sh_il_shlr8(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(0, SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(8)));
}

/**
 * SHLL16  Rn
 * Rn << 16 -> Rn
 * 0100nnnn00101000
 */
static RzILOpEffect *sh_il_shll16(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(0, SHIFTL0(sh_il_get_pure_param(0), SH_U_REG(16)));
}

/**
 * SHLR16  Rn
 * Rn >> 16 -> Rn
 * 0100nnnn00101001
 */
static RzILOpEffect *sh_il_shlr16(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(0, SHIFTR0(sh_il_get_pure_param(0), SH_U_REG(16)));
}

/**
 * BF  label
 * if T = 0, disp * 2 + PC + 4 -> PC ; otherwise (T = 1) NOP
 * 10001011dddddddd
 */
static RzILOpEffect *sh_il_bf(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *new_pc = sh_il_get_effective_addr_param(0);
	return BRANCH(IS_ZERO(sh_il_get_status_reg_bit(SH_SR_T)), JMP(new_pc), NOP());
}

/**
 * BF/S  label
 * if T = 0, disp * 2 + PC + 4 -> PC ; otherwise (T = 1) NOP ; delayed branch
 * 10001111dddddddd
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_bfs(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *new_pc = sh_il_get_effective_addr_param(0);
	return BRANCH(IS_ZERO(sh_il_get_status_reg_bit(SH_SR_T)), JMP(new_pc), NOP());
}

/**
 * BT  label
 * if T = 1, disp * 2 + PC + 4 -> PC ; otherwise (T = 0) NOP
 * 10001001dddddddd
 */
static RzILOpEffect *sh_il_bt(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *new_pc = sh_il_get_effective_addr_param(0);
	return BRANCH(VARG(SH_SR_T), JMP(new_pc), NOP());
}

/**
 * BT/S  label
 * if T = 1, disp * 2 + PC + 4 -> PC ; otherwise (T = 0) NOP ; delayed branch
 * 10001101dddddddd
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_bts(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzILOpPure *new_pc = sh_il_get_effective_addr_param(0);
	return BRANCH(IS_ZERO(sh_il_get_status_reg_bit(SH_SR_T)), NOP(), JMP(new_pc));
}

/**
 * BRA  label
 * disp * 2 + PC + 4 -> PC ; delayed branch
 * 1010dddddddddddd
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_bra(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return JMP(sh_il_get_effective_addr_param(0));
}

/**
 * BRAF  Rn
 * Rn + PC + 4 -> PC ; delayed branch
 * 0000nnnn00100011
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_braf(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return JMP(sh_il_get_effective_addr_param(0));
}

/**
 * BSR  label
 * PC + 4 -> PR ; disp * 2 + PC + 4 -> PC ; delayed branch
 * 1011dddddddddddd
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_bsr(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SEQ2(SETG("pr", SH_U_ADDR(pc)), JMP(sh_il_get_effective_addr_param(0)));
}

/**
 * BSRF  Rn
 * PC + 4 -> PR ; Rn + PC + 4 -> PC ; delayed branch
 * 0000nnnn00000011
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_bsrf(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SEQ2(SETG("pr", SH_U_ADDR(pc)), JMP(sh_il_get_effective_addr_param(0)));
}

/**
 * JMP  @Rn
 * Rn -> PC ; delayed branch
 * 0100nnnn00101011
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_jmp(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return JMP(sh_il_get_effective_addr_param(0));
}

/**
 * JSR  @Rn
 * PC + 4 -> PR ; Rn -> PC ; delayed branch
 * 0100nnnn00001011
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_jsr(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SEQ2(SETG("pr", ADD(SH_U_ADDR(pc), SH_U_ADDR(4))), JMP(sh_il_get_effective_addr_param(0)));
}

/**
 * RTS
 * PR -> PC ; delayed branch
 * 0000000000001011
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_rts(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return JMP(VARG("pr"));
}

/**
 * CLRMAC
 * 0 -> MACH, MACL
 * 0000000000101000
 */
static RzILOpEffect *sh_il_clrmac(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SEQ2(SETG("mach", UN(SH_REG_SIZE, 0)), SETG("macl", UN(SH_REG_SIZE, 0)));
}

/**
 * CLRS
 * 0 -> S
 * 0000000001001000
 */
static RzILOpEffect *sh_il_clrs(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_S, IL_FALSE);
}

/**
 * CLRT
 * 0 -> T
 * 0000000000001000
 */
static RzILOpEffect *sh_il_clrt(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, IL_FALSE);
}

/**
 * LDC  Rm, REG
 * REG := SR/GBR/VBR/SSR/SPC/DBR/Rn_BANK
 * Rm -> REG
 * PRIVILEGED (Only GBR is not privileged)
 *
 * LDC.L  @Rm+, REG
 * REG := SR/GBR/VBR/SSR/SPC/DBR/Rn_BANK
 * (Rm) -> REG ; Rm + 4 -> Rm
 * PRIVILEGED (Only GBR is not privileged)
 */
static RzILOpEffect *sh_il_ldc(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzBitVector *priv_bit = rz_il_evaluate_bitv(analysis->il_vm->vm, sh_il_get_status_reg_bit(SH_SR_D));
	ut8 state = priv_bit->bits.small_u == 0 ? 0x1 : 0x0;
	state += op->param[1].param[0] != SH_REG_IND_GBR ? 0x2 : 0x0;
	if (state == 0x3) {
		rz_il_vm_event_add(analysis->il_vm->vm, rz_il_event_exception_new("SuperH: RESINST"));
		return NULL;
	}
	if (op->scaling == SH_SCALING_INVALID) {
		if (sh_valid_gpr(op->param[1].param[0])) {
			return SETG(sh_get_banked_reg(op->param[1].param[0], 1), sh_il_get_pure_param(0));
		} else {
			return sh_il_set_pure_param(1, sh_il_get_pure_param(0));
		}
	} else if (op->scaling == SH_SCALING_L) {
		SHParamHelper rm = sh_il_get_param(op->param[0], op->scaling);
		if (sh_valid_gpr(op->param[1].param[0])) {
			return SEQ2(SETG(sh_get_banked_reg(op->param[1].param[0], 1), rm.pure), rm.post);
		} else {
			return SEQ2(sh_il_set_pure_param(1, rm.pure), rm.post);
		}
	}
	return NOP();
}

/**
 * LDS  Rm, REG
 * REG := MACH/MACL/PR
 * Rm -> REG
 *
 * LDS.L  @Rm+, REG
 * REG := MACH/MACL/PR
 * (Rm) -> REG ; Rm + 4 -> Rm
 */
static RzILOpEffect *sh_il_lds(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	if (op->scaling == SH_SCALING_INVALID) {
		return sh_il_set_pure_param(1, sh_il_get_pure_param(0));
	} else if (op->scaling == SH_SCALING_L) {
		SHParamHelper rm = sh_il_get_param(op->param[0], op->scaling);
		return SEQ2(sh_il_set_pure_param(1, rm.pure), rm.post);
	}
	return NOP();
}

/**
 * MOVCA.L  R0, @Rn
 * R0 -> (Rn) (without fetching cache block)
 * 0000nnnn11000011
 */
static RzILOpEffect *sh_il_movca(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, sh_il_get_pure_param(0));
}

/**
 * NOP
 * No operation
 * 0000000000001001
 */
static RzILOpEffect *sh_il_nop(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return NOP();
}

/**
 * RTE
 * SSR -> SR ; SPC -> PC ; delayed branch
 * 0000000000101011
 * PRIVILEGED
 * TODO: Implement delayed branch
 */
static RzILOpEffect *sh_il_rte(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzBitVector *priv_bit = rz_il_evaluate_bitv(analysis->il_vm->vm, sh_il_get_status_reg_bit(SH_SR_D));
	if (priv_bit->bits.small_u == 0) {
		rz_il_vm_event_add(analysis->il_vm->vm, rz_il_event_exception_new("SuperH: RESINST"));
		return NULL;
	}
	return SEQ2(sh_il_set_status_reg(VARG("ssr")), JMP(VARG("spc")));
}

/**
 * SETS
 * 1 -> S
 * 0000000001011000
 */
static RzILOpEffect *sh_il_sets(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_S, IL_TRUE);
}

/**
 * SETT
 * 1 -> T
 * 0000000000011000
 */
static RzILOpEffect *sh_il_sett(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return SETG(SH_SR_T, IL_TRUE);
}

/**
 * SLEEP
 * Sleep or standby (so effectively, just a NOP)
 * 0000000000011011
 * PRIVILEGED
 */
static RzILOpEffect *sh_il_sleep(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzBitVector *priv_bit = rz_il_evaluate_bitv(analysis->il_vm->vm, sh_il_get_status_reg_bit(SH_SR_D));
	if (priv_bit->bits.small_u == 0) {
		rz_il_vm_event_add(analysis->il_vm->vm, rz_il_event_exception_new("SuperH: RESINST"));
		return NULL;
	}
	return NOP();
}

/**
 * STC  REG, Rn
 * REG := SR/GBR/VBR/SSR/SPC/SGR/DBR/Rn_BANK
 * REG -> Rn
 * PRIVILEGED (Only GBR is not privileged)
 *
 * STC.L  REG, @-Rn
 * REG := SR/GBR/VBR/SSR/SPC/SGR/DBR/Rn_BANK
 * Rn - 4 -> Rn ; REG -> (Rn)
 * PRIVILEGED (Only GBR is not privileged)
 */
static RzILOpEffect *sh_il_stc(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RzBitVector *priv_bit = rz_il_evaluate_bitv(analysis->il_vm->vm, sh_il_get_status_reg_bit(SH_SR_D));
	ut8 state = priv_bit->bits.small_u == 0 ? 0x1 : 0x0;
	state += op->param[0].param[0] != SH_REG_IND_GBR ? 0x2 : 0x0;
	if (state == 0x3) { // REG != GBR and user mode
		rz_il_vm_event_add(analysis->il_vm->vm, rz_il_event_exception_new("SuperH: RESINST"));
		return NULL;
	}
	if (sh_valid_gpr(op->param[0].param[0])) { // REG = Rn_BANK
		return sh_il_set_pure_param(1, VARG(sh_get_banked_reg(op->param[0].param[0], 1)));
	} else {
		return sh_il_set_pure_param(1, sh_il_get_pure_param(0));
	}
	return NOP();
}

/**
 * STS  REG, Rn
 * REG := MACH/MACL/PR
 * REG -> Rn
 *
 * STS.L  REG, @-Rn
 * REG := MACH/MACL/PR
 * Rn + 4 -> Rn ; REG -> (Rn)
 */
static RzILOpEffect *sh_il_sts(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	return sh_il_set_pure_param(1, sh_il_get_pure_param(0));
}

static RzILOpEffect *sh_il_unimpl(SHOp *op, ut64 pc, RzAnalysis *analysis) {
	RZ_LOG_WARN("SuperH: Instruction with opcode 0x%04x is unimplemented\n", op->opcode);
	return EMPTY();
}

#include <rz_il/rz_il_opbuilder_end.h>

typedef RzILOpEffect *(*sh_il_op)(SHOp *aop, ut64 pc, RzAnalysis *analysis);

static sh_il_op sh_ops[SH_OP_SIZE] = {
	[SH_OP_INVALID] = sh_il_invalid,
	[SH_OP_MOV] = sh_il_mov,
	[SH_OP_MOVT] = sh_il_movt,
	[SH_OP_SWAP] = sh_il_swap,
	[SH_OP_XTRCT] = sh_il_xtrct,
	[SH_OP_ADD] = sh_il_add,
	[SH_OP_ADDC] = sh_il_addc,
	[SH_OP_ADDV] = sh_il_addv,
	[SH_OP_CMP_EQ] = sh_il_cmp_eq,
	[SH_OP_CMP_HS] = sh_il_cmp_hs,
	[SH_OP_CMP_GE] = sh_il_cmp_ge,
	[SH_OP_CMP_HI] = sh_il_cmp_hi,
	[SH_OP_CMP_GT] = sh_il_cmp_gt,
	[SH_OP_CMP_PZ] = sh_il_cmp_pz,
	[SH_OP_CMP_PL] = sh_il_cmp_pl,
	[SH_OP_CMP_STR] = sh_il_cmp_str,
	[SH_OP_DIV1] = sh_il_div1,
	[SH_OP_DIV0S] = sh_il_div0s,
	[SH_OP_DIV0U] = sh_il_div0u,
	[SH_OP_DMULS] = sh_il_dmuls,
	[SH_OP_DMULU] = sh_il_dmulu,
	[SH_OP_DT] = sh_il_dt,
	[SH_OP_EXTS] = sh_il_exts,
	[SH_OP_EXTU] = sh_il_extu,
	[SH_OP_MAC] = sh_il_mac,
	[SH_OP_MUL] = sh_il_mul,
	[SH_OP_MULS] = sh_il_muls,
	[SH_OP_MULU] = sh_il_mulu,
	[SH_OP_NEG] = sh_il_neg,
	[SH_OP_NEGC] = sh_il_negc,
	[SH_OP_SUB] = sh_il_sub,
	[SH_OP_SUBC] = sh_il_subc,
	[SH_OP_SUBV] = sh_il_subv,
	[SH_OP_AND] = sh_il_and,
	[SH_OP_NOT] = sh_il_not,
	[SH_OP_OR] = sh_il_or,
	[SH_OP_TAS] = sh_il_tas,
	[SH_OP_TST] = sh_il_tst,
	[SH_OP_XOR] = sh_il_xor,
	[SH_OP_ROTL] = sh_il_rotl,
	[SH_OP_ROTR] = sh_il_rotr,
	[SH_OP_ROTCL] = sh_il_rotcl,
	[SH_OP_ROTCR] = sh_il_rotcr,
	[SH_OP_SHAD] = sh_il_shad,
	[SH_OP_SHAL] = sh_il_shal,
	[SH_OP_SHAR] = sh_il_shar,
	[SH_OP_SHLD] = sh_il_shld,
	[SH_OP_SHLL] = sh_il_shll,
	[SH_OP_SHLR] = sh_il_shlr,
	[SH_OP_SHLL2] = sh_il_shll2,
	[SH_OP_SHLR2] = sh_il_shlr2,
	[SH_OP_SHLL8] = sh_il_shll8,
	[SH_OP_SHLR8] = sh_il_shlr8,
	[SH_OP_SHLL16] = sh_il_shll16,
	[SH_OP_SHLR16] = sh_il_shlr16,
	[SH_OP_BF] = sh_il_bf,
	[SH_OP_BFS] = sh_il_bfs,
	[SH_OP_BT] = sh_il_bt,
	[SH_OP_BTS] = sh_il_bts,
	[SH_OP_BRA] = sh_il_bra,
	[SH_OP_BRAF] = sh_il_braf,
	[SH_OP_BSR] = sh_il_bsr,
	[SH_OP_BSRF] = sh_il_bsrf,
	[SH_OP_JMP] = sh_il_jmp,
	[SH_OP_JSR] = sh_il_jsr,
	[SH_OP_RTS] = sh_il_rts,
	[SH_OP_CLRMAC] = sh_il_clrmac,
	[SH_OP_CLRS] = sh_il_clrs,
	[SH_OP_CLRT] = sh_il_clrt,
	[SH_OP_LDC] = sh_il_ldc,
	[SH_OP_LDS] = sh_il_lds,
	[SH_OP_MOVCA] = sh_il_movca,
	[SH_OP_NOP] = sh_il_nop,
	[SH_OP_RTE] = sh_il_rte,
	[SH_OP_SETS] = sh_il_sets,
	[SH_OP_SETT] = sh_il_sett,
	[SH_OP_SLEEP] = sh_il_sleep,
	[SH_OP_STC] = sh_il_stc,
	[SH_OP_STS] = sh_il_sts,
	[SH_OP_UNIMPL] = sh_il_unimpl
};

RZ_IPI bool rz_sh_il_opcode(RZ_NONNULL RzAnalysis *analysis, RZ_NONNULL RzAnalysisOp *aop, ut64 pc, RZ_BORROW RZ_NONNULL SHOp *op) {
	rz_return_val_if_fail(analysis && aop && op, false);
	if (op->mnemonic >= SH_OP_SIZE) {
		RZ_LOG_ERROR("RzIL: SuperH: out of bounds op\n");
		return false;
	}

	sh_il_op create_op = sh_ops[op->mnemonic];
	aop->il_op = create_op(op, pc, analysis);

	return true;
}

RZ_IPI RzAnalysisILConfig *rz_sh_il_config(RZ_NONNULL RzAnalysis *analysis) {
	rz_return_val_if_fail(analysis, NULL);

	RzAnalysisILConfig *r = rz_analysis_il_config_new(SH_ADDR_SIZE, analysis->big_endian, SH_ADDR_SIZE);
	return r;
}