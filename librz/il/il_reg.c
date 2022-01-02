// SPDX-FileCopyrightText: 2021 Florian Märkl <info@florianmaerkl.de>
// SPDX-License-Identifier: LGPL-3.0-only

#include <rz_il/rz_il_reg.h>
#include <rz_il/rz_il_vm.h>
#include <rz_util.h>

static int reg_offset_cmp(const void *value, const void *list_data) {
	return ((RzRegItem *)value)->offset - ((RzRegItem *)list_data)->offset;
}

static void reg_binding_item_fini(RzILRegBindingItem *item, void *unused) {
	free(item->name);
}

/**
 * \brief Calculate a new binding of IL variables against the profile of the given RzReg
 *
 * Because registers can overlap, not all registers may get a binding.
 * Informally, only the "larger" ones, containing "smaller" ones are bound,
 * except for 1-bit registers, which are always preferred.
 *
 * More specifically, the set of registers to be bound is determined like this:
 * First, bind all 1-bit registers (flags).
 * Then, bind a (sub)set of the remaining registers like this:
 * * Begin with the set of all registers.
 * * Remove all registers overlapping with an already-bound 1-bit register.
 * * Remove all registers that are covered entirely by another register in the same set and are smaller than it.
 * * Remove the one marked with RZ_REG_NAME_PC, if it exists.
 * * While there still exists at least overlap, from the overlap of two registers at the lowest offset,
 *   remove the register with the higher offset.
 *
 * If two registers have the same offset and size, the result is currently undefined.
 */
RZ_API RzILRegBinding *rz_il_reg_binding_derive(RZ_NONNULL RzReg *reg) {
	rz_return_val_if_fail(reg, NULL);
	RzILRegBinding *rb = RZ_NEW0(RzILRegBinding);
	if (!rb) {
		return NULL;
	}
	RzVector regs;
	rz_vector_init(&regs, sizeof(RzILRegBindingItem), (RzVectorFree)reg_binding_item_fini, NULL);
	for (int i = 0; i < RZ_REG_TYPE_LAST; i++) {
		// bind all flags (1-bit regs) unconditionally
		RzRegItem *item;
		RzListIter *iter;
		RzList *flags = rz_list_new();
		if (!flags) {
			continue;
		}
		rz_list_foreach (reg->regset[i].regs, iter, item) {
			if (item->size != 1) {
				continue;
			}
			// check for same-offset flag
			RzRegItem *item2;
			RzListIter *iter2;
			rz_list_foreach (flags, iter2, item2) {
				if (item2->offset == item->offset) {
					goto next_flag;
				}
			}
			// all good, bind it
			rz_list_push(flags, item);
			char *name = strdup(item->name);
			if (!name) {
				rz_list_free(flags);
				goto err;
			}
			RzILRegBindingItem *bitem = rz_vector_push(&regs, NULL);
			if (!bitem) {
				free(name);
				rz_list_free(flags);
				goto err;
			}
			bitem->name = name;
			bitem->size = item->size;
		next_flag:
			continue;
		}
		// for the remaining regs, first filter regs that contain a flag
		RzList *nonflags = rz_list_new();
		if (!nonflags) {
			rz_list_free(flags);
			goto err;
		}
		rz_list_foreach (reg->regset[i].regs, iter, item) {
			RzRegItem *flag;
			RzListIter *fiter;
			rz_list_foreach (flags, fiter, flag) {
				if (flag->offset >= item->offset && flag->offset < item->offset + item->size) {
					goto next_reg;
				}
			}
			rz_list_push(nonflags, item);
		next_reg:
			continue;
		}
		// then bind the remaining regs, favoring larger ones on overlaps
		RzList *items = rz_reg_filter_items_covered(nonflags);
		rz_list_free(nonflags);
		if (!items) {
			rz_list_free(flags);
			continue;
		}
		rz_list_sort(items, reg_offset_cmp);
		const char *pc = rz_reg_get_name(reg, RZ_REG_NAME_PC);
		RzRegItem *prev = NULL;
		rz_list_foreach (items, iter, item) {
			if (prev && prev->offset + prev->size > item->offset) {
				// overlap where one reg is not fully contained in another.
				// this is not supported yet.
				continue;
			}
			if (pc && !strcmp(item->name, pc)) {
				// pc is handled outside of reg binding
				continue;
			}
			char *name = strdup(item->name);
			if (!name) {
				rz_list_free(flags);
				rz_list_free(items);
				goto err;
			}
			RzILRegBindingItem *bitem = rz_vector_push(&regs, NULL);
			if (!bitem) {
				free(name);
				rz_list_free(flags);
				rz_list_free(items);
				goto err;
			}
			bitem->name = name;
			bitem->size = item->size;
			prev = item;
		}
		rz_list_free(items);
		rz_list_free(flags);
	}
	// from now on, the array should be treated immutable, so we deliberately don't use RzVector anymore.
	rb->regs_count = rz_vector_len(&regs);
	rb->regs = rz_vector_flush(&regs);
	rz_vector_fini(&regs);
	return rb;
err:
	rz_vector_fini(&regs);
	free(rb);
	return NULL;
}

/**
 * Create a new binding that binds exactly the given register names, querying \p reg for any additionally needed info
 * \param regs array of \p regs_count names of registers. Each of these must be part of \p reg.
 */
RZ_API RzILRegBinding *rz_il_reg_binding_exactly(RZ_NONNULL RzReg *reg, size_t regs_count, RZ_NONNULL RZ_BORROW const char **regs) {
	rz_return_val_if_fail(reg && regs, NULL);
	RzILRegBinding *rb = RZ_NEW(RzILRegBinding);
	if (!rb) {
		return NULL;
	}
	rb->regs_count = regs_count;
	rb->regs = calloc(regs_count, sizeof(RzILRegBindingItem));
	if (!rb->regs) {
		goto err_rb;
	}
	for (size_t i = 0; i < regs_count; i++) {
		RzRegItem *ri = rz_reg_get(reg, regs[i], RZ_REG_TYPE_ANY);
		if (!ri) {
			goto err_regs;
		}
		rb->regs[i].name = strdup(regs[i]);
		if (!rb->regs[i].name) {
			goto err_regs;
		}
		rb->regs[i].size = ri->size;
	}
	return rb;
err_regs:
	for (size_t i = 0; i < regs_count; i++) {
		reg_binding_item_fini(&rb->regs[i], NULL);
	}
	free(rb->regs);
err_rb:
	free(rb);
	return NULL;
}

RZ_API void rz_il_reg_binding_free(RzILRegBinding *rb) {
	if (!rb) {
		return;
	}
	for (size_t i = 0; i < rb->regs_count; i++) {
		reg_binding_item_fini(&rb->regs[i], NULL);
	}
	free(rb->regs);
	free(rb);
}

/**
 * Setup variables to bind against registers
 * \p rb the binding for which to create variables, ownership is transferred to the vm.
 */
RZ_API void rz_il_vm_setup_reg_binding(RZ_NONNULL RzILVM *vm, RZ_NONNULL RZ_OWN RzILRegBinding *rb) {
	rz_return_if_fail(vm && rb && !vm->reg_binding);
	vm->reg_binding = rb;
	for (size_t i = 0; i < rb->regs_count; i++) {
		rz_il_vm_add_reg(vm, rb->regs[i].name, rb->regs[i].size);
	}
}

/**
 * Set the values of all bound regs in \p reg to the respective variable or PC contents in \p vm.
 *
 * Contents of unbound registers are left unchanged (unless they overlap with bound registers).
 *
 * If for example the register profile used for \p reg does not match the one used to build the initial binding,
 * different errors might happen, e.g. a register size might not match the variable's value size.
 * In such cases, this function still applies everything it can, zero-extending or cropping values where necessary.
 *
 * \return whether the sync was cleanly applied without errors or adjustments
 */
RZ_API bool rz_il_vm_sync_to_reg(RZ_NONNULL RzILVM *vm, RZ_NONNULL RzReg *reg) {
	rz_return_val_if_fail(vm && reg, false);
	bool perfect = true;
	const char *pc = rz_reg_get_name(reg, RZ_REG_NAME_PC);
	if (pc) {
		RzRegItem *ri = rz_reg_get(reg, pc, RZ_REG_TYPE_ANY);
		if (ri) {
			RzBitVector *pcbv = rz_bv_new_zero(ri->size);
			if (pcbv) {
				perfect &= rz_bv_len(pcbv) == rz_bv_len(vm->pc);
				rz_bv_copy_nbits(vm->pc, 0, pcbv, 0, RZ_MIN(rz_bv_len(pcbv), rz_bv_len(vm->pc)));
				rz_reg_set_bv(reg, ri, pcbv);
				rz_bv_free(pcbv);
			} else {
				perfect = false;
			}
		} else {
			perfect = false;
		}
	} else {
		perfect = false;
	}
	RzILRegBinding *rb = vm->reg_binding;
	if (!vm->reg_binding) {
		return false;
	}
	for (size_t i = 0; i < rb->regs_count; i++) {
		RzILRegBindingItem *item = &rb->regs[i];
		RzRegItem *ri = rz_reg_get(reg, item->name, RZ_REG_TYPE_ANY);
		if (!ri) {
			perfect = false;
			continue;
		}
		RzILVal *val = rz_il_hash_find_val_by_name(vm, item->name);
		if (!val || val->type != RZIL_VAR_TYPE_BV) {
			perfect = false;
			RzBitVector *bv = rz_bv_new_zero(ri->size);
			if (!bv) {
				break;
			}
			if (bv) {
				rz_reg_set_bv(reg, ri, bv);
				rz_bv_free(bv);
			}
			continue;
		}
		RzBitVector *dupped = NULL;
		const RzBitVector *bv = val->data.bv;
		if (rz_bv_len(bv) != ri->size) {
			perfect = false;
			dupped = rz_bv_new_zero(ri->size);
			if (!dupped) {
				break;
			}
			rz_bv_copy_nbits(bv, 0, dupped, 0, RZ_MIN(rz_bv_len(bv), ri->size));
			bv = dupped;
		}
		perfect &= rz_reg_set_bv(reg, ri, bv);
		rz_bv_free(dupped);
	}
	return perfect;
}

/**
 * Set the values of all variables in \p vm that are bound to registers and PC to the respective contents from \p reg.
 * Contents of variables that are not bound to a register are left unchanged.
 */
RZ_API void rz_il_vm_sync_from_reg(RzILVM *vm, RZ_NONNULL RzReg *reg) {
	rz_return_if_fail(vm && reg);
	const char *pc = rz_reg_get_name(reg, RZ_REG_NAME_PC);
	if (pc) {
		RzRegItem *ri = rz_reg_get(reg, pc, RZ_REG_TYPE_ANY);
		if (ri) {
			rz_bv_set_all(vm->pc, 0);
			RzBitVector *pcbv = rz_reg_get_bv(reg, ri);
			if (pcbv) {
				rz_bv_copy_nbits(pcbv, 0, vm->pc, 0, RZ_MIN(rz_bv_len(pcbv), rz_bv_len(vm->pc)));
				rz_bv_free(pcbv);
			}
		}
	}
	RzILRegBinding *rb = vm->reg_binding;
	if (!vm->reg_binding) {
		return;
	}
	for (size_t i = 0; i < rb->regs_count; i++) {
		RzILRegBindingItem *item = &rb->regs[i];
		RzILVar *var = rz_il_find_var_by_name(vm, item->name);
		if (!var) {
			RZ_LOG_ERROR("IL Variable \"%s\" does not exist for bound register of the same name.\n", item->name);
			continue;
		}
		RzRegItem *ri = rz_reg_get(reg, item->name, RZ_REG_TYPE_ANY);
		RzBitVector *bv = ri ? rz_reg_get_bv(reg, ri) : rz_bv_new_zero(item->size);
		if (!bv) {
			continue;
		}
		RzBitVector *dupped = NULL;
		if (rz_bv_len(bv) != item->size) {
			dupped = rz_bv_new_zero(item->size);
			if (!dupped) {
				break;
			}
			rz_bv_copy_nbits(bv, 0, dupped, 0, RZ_MIN(rz_bv_len(bv), item->size));
			bv = dupped;
		}
		rz_il_hash_bind(vm, var, rz_il_vm_fortify_bitv(vm, bv));
		rz_bv_free(dupped);
	}
}