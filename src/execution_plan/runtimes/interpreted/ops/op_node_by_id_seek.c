/*
* Copyright 2018-2021 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "op_node_by_id_seek.h"
#include "RG.h"
#include "../../../../query_ctx.h"

/* Forward declarations. */
static RT_OpResult NodeByIdSeekInit(RT_OpBase *opBase);
static Record NodeByIdSeekConsume(RT_OpBase *opBase);
static Record NodeByIdSeekConsumeFromChild(RT_OpBase *opBase);
static RT_OpResult NodeByIdSeekReset(RT_OpBase *opBase);
static RT_OpBase *NodeByIdSeekClone(const RT_ExecutionPlan *plan, const RT_OpBase *opBase);
static void NodeByIdSeekFree(RT_OpBase *opBase);

// Checks to see if operation index is within its bounds.
static inline bool _outOfBounds(RT_NodeByIdSeek *op) {
	/* Because currentId starts at minimum and only increases
	 * we only care about top bound. */
	if(op->currentId > op->maxId) return true;
	return false;
}

RT_OpBase *RT_NewNodeByIdSeekOp(const RT_ExecutionPlan *plan, const char *alias, UnsignedRange *id_range) {

	RT_NodeByIdSeek *op = rm_malloc(sizeof(RT_NodeByIdSeek));
	op->g = QueryCtx_GetGraph();
	op->child_record = NULL;
	op->alias = alias;

	op->minId = id_range->include_min ? id_range->min : id_range->min + 1;
	/* The largest possible entity ID is the same as Graph_RequiredMatrixDim.
	 * This value will be set on Init, to allow operation clone be independent
	 * on the current graph size.*/
	op->maxId = id_range->include_max ? id_range->max : id_range->max - 1;

	op->currentId = op->minId;

	RT_OpBase_Init((RT_OpBase *)op, OPType_NODE_BY_ID_SEEK, NodeByIdSeekInit,
				NodeByIdSeekConsume, NodeByIdSeekReset, NodeByIdSeekClone, NodeByIdSeekFree,
				false, plan);

	op->nodeRecIdx = OpBase_Modifies((OpBase *)op, alias);

	return (RT_OpBase *)op;
}

static RT_OpResult NodeByIdSeekInit(RT_OpBase *opBase) {
	ASSERT(opBase->type == OPType_NODE_BY_ID_SEEK);
	RT_NodeByIdSeek *op = (RT_NodeByIdSeek *)opBase;
	// The largest possible entity ID is the number of nodes - deleted and real - in the DataBlock.
	size_t node_count = Graph_UncompactedNodeCount(op->g);
	op->maxId = MIN(node_count - 1, op->maxId);
	if(opBase->childCount > 0) RT_OpBase_UpdateConsume(opBase, NodeByIdSeekConsumeFromChild);
	return OP_OK;
}

static inline Node _SeekNextNode(RT_NodeByIdSeek *op) {
	Node n = GE_NEW_NODE();

	/* As long as we're within range bounds
	 * and we've yet to get a node. */
	while(!_outOfBounds(op)) {
		if(Graph_GetNode(op->g, op->currentId, &n)) break;
		op->currentId++;
	}

	// Advance id for next consume call.
	op->currentId++;

	return n;
}

static Record NodeByIdSeekConsumeFromChild(RT_OpBase *opBase) {
	RT_NodeByIdSeek *op = (RT_NodeByIdSeek *)opBase;

	if(op->child_record == NULL) {
		op->child_record = RT_OpBase_Consume(op->op.children[0]);
		if(op->child_record == NULL) return NULL;
		else NodeByIdSeekReset(opBase);
	}

	Node n = _SeekNextNode(op);

	if(n.entity == NULL) { // Failed to retrieve a node.
		RT_OpBase_DeleteRecord(op->child_record); // Free old record.
		// Pull a new record from child.
		op->child_record = RT_OpBase_Consume(op->op.children[0]);
		if(op->child_record == NULL) return NULL; // Child depleted.

		// Reset iterator and evaluate again.
		NodeByIdSeekReset(opBase);
		n = _SeekNextNode(op);
		if(n.entity == NULL) return NULL; // Empty iterator; return immediately.
	}

	// Clone the held Record, as it will be freed upstream.
	Record r = RT_OpBase_CloneRecord(op->child_record);

	// Populate the Record with the actual node.
	Record_AddNode(r, op->nodeRecIdx, n);

	return r;
}

static Record NodeByIdSeekConsume(RT_OpBase *opBase) {
	RT_NodeByIdSeek *op = (RT_NodeByIdSeek *)opBase;

	Node n = _SeekNextNode(op);
	if(n.entity == NULL) return NULL; // Failed to retrieve a node.

	// Create a new Record.
	Record r = RT_OpBase_CreateRecord(opBase);

	// Populate the Record with the actual node.
	Record_AddNode(r, op->nodeRecIdx, n);

	return r;
}

static RT_OpResult NodeByIdSeekReset(RT_OpBase *ctx) {
	RT_NodeByIdSeek *op = (RT_NodeByIdSeek *)ctx;
	op->currentId = op->minId;
	return OP_OK;
}

static RT_OpBase *NodeByIdSeekClone(const RT_ExecutionPlan *plan, const RT_OpBase *opBase) {
	ASSERT(opBase->type == OPType_NODE_BY_ID_SEEK);
	RT_NodeByIdSeek *op = (RT_NodeByIdSeek *)opBase;
	UnsignedRange range;
	range.min = op->minId;
	range.max = op->maxId;
	/* In order to clone the range set at the original op, the range must be inclusive so the clone will set the exact range as the original.
	 * During the call to NewNodeByIdSeekOp with the range, the following lines are executed:
	 * op->minId = id_range->include_min ? id_range->min : id_range->min + 1;
	 * op->maxId = id_range->include_max ? id_range->max : id_range->max - 1;
	 * Since the range object min equals to the origin minId, and so is the max equals to the origin maxId, and the range is inclusive
	 * the clone will set its values to be the same as in the origin. */
	range.include_min = true;
	range.include_max = true;
	return RT_NewNodeByIdSeekOp(plan, op->alias, &range);
}

static void NodeByIdSeekFree(RT_OpBase *opBase) {
	RT_NodeByIdSeek *op = (RT_NodeByIdSeek *)opBase;
	if(op->child_record) {
		RT_OpBase_DeleteRecord(op->child_record);
		op->child_record = NULL;
	}
}