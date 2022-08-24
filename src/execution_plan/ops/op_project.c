/*
 * Copyright 2018-2022 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#include "op_project.h"
#include "RG.h"
#include "op_sort.h"
#include "../../util/arr.h"
#include "../../query_ctx.h"
#include "../../util/rmalloc.h"

// forward declarations
static Record ProjectConsume(OpBase *opBase);
static OpResult ProjectReset(OpBase *opBase);
static OpBase *ProjectClone(const ExecutionPlan *plan, const OpBase *opBase);
static void ProjectFree(OpBase *opBase);

OpBase *NewProjectOp
(
	const ExecutionPlan *plan,
	AR_ExpNode **exps
) {
	OpProject *op = rm_malloc(sizeof(OpProject));
	op->exps = exps;
	op->singleResponse = false;
	op->exp_count = array_len(exps);
	op->record_offsets = array_new(uint, op->exp_count);
	op->r = NULL;
	op->projection = NULL;

	// set our op operations
	OpBase_Init((OpBase *)op, OPType_PROJECT, "Project", NULL, ProjectConsume,
				ProjectReset, NULL, ProjectClone, ProjectFree, false, plan);

	for(uint i = 0; i < op->exp_count; i ++) {
		// the projected record will associate values with their resolved name
		// to ensure that space is allocated for each entry
		int record_idx = OpBase_Modifies((OpBase *)op,
				op->exps[i]->resolved_name);
		array_append(op->record_offsets, record_idx);
	}

	return (OpBase *)op;
}

static Record ProjectConsume(OpBase *opBase) {
	OpProject *op = (OpProject *)opBase;

	if(op->op.childCount) {
		OpBase *child = op->op.children[0];
		op->r = OpBase_Consume(child);
		if(!op->r) return NULL;
	} else {
		// QUERY: RETURN 1+2
		// Return a single record followed by NULL on the second call.
		if(op->singleResponse) return NULL;
		op->singleResponse = true;
		op->r = OpBase_CreateRecord(opBase);
	}

	op->projection = OpBase_CreateRecord(opBase);

	for(uint i = 0; i < op->exp_count; i++) {
		AR_ExpNode *exp = op->exps[i];
		SIValue v = AR_EXP_Evaluate(exp, op->r);
		int rec_idx = op->record_offsets[i];
		// persisting a value is only necessary here
		// if 'v' refers to a scalar held in Record 'r'
		// graph entities don't need to be persisted here
		// as Record_Add will copy them internally
		// the RETURN projection here requires persistence:
		// MATCH (a) WITH toUpper(a.name) AS e RETURN e
		// TODO: this is a rare case;
		// the logic of when to persist can be improved
		if(!(v.type & SI_GRAPHENTITY)) {
			SIValue_Persist(&v);
		}
		Record_Add(op->projection, rec_idx, v);
		// if the value was a graph entity with its own allocation
		// as with a query like:
		// MATCH p = (src) RETURN nodes(p)[0]
		// ensure that the allocation is freed here
		if((v.type & SI_GRAPHENTITY)) {
			SIValue_Free(v);
		}
	}

	OpBase_DeleteRecord(op->r);
	op->r = NULL;

	// emit the projected record once
	Record projection = op->projection;
	op->projection = NULL;
	return projection;
}

static OpResult ProjectReset(OpBase *opBase) {
	OpProject *op = (OpProject *)opBase;
	op->singleResponse = false;
	return OP_OK;
}

static OpBase *ProjectClone(const ExecutionPlan *plan, const OpBase *opBase) {
	ASSERT(opBase->type == OPType_PROJECT);
	OpProject *op = (OpProject *)opBase;
	AR_ExpNode **exps;
	array_clone_with_cb(exps, op->exps, AR_EXP_Clone);
	return NewProjectOp(plan, exps);
}

static void ProjectFree(OpBase *ctx) {
	OpProject *op = (OpProject *)ctx;

	if(op->exps) {
		for(uint i = 0; i < op->exp_count; i ++) AR_EXP_Free(op->exps[i]);
		array_free(op->exps);
		op->exps = NULL;
	}

	if(op->record_offsets) {
		array_free(op->record_offsets);
		op->record_offsets = NULL;
	}

	if(op->r) {
		OpBase_DeleteRecord(op->r);
		op->r = NULL;
	}

	if(op->projection) {
		OpBase_DeleteRecord(op->projection);
		op->projection = NULL;
	}
}

