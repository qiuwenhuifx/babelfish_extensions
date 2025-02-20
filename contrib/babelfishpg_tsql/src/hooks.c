#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "access/xact.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_attrdef_d.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/explain.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_utilcmd.h"
#include "parser/parse_target.h"
#include "parser/parser.h"
#include "parser/scanner.h"
#include "parser/scansup.h"
#include "replication/logical.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/numeric.h"
#include <math.h>

#include "pltsql.h"
#include "backend_parser/scanner.h"
#include "hooks.h"
#include "pltsql.h"
#include "pl_explain.h"
#include "catalog.h"
#include "rolecmds.h"
#include "session.h"

#define TDS_NUMERIC_MAX_PRECISION	38

extern bool babelfish_dump_restore;
extern bool pltsql_quoted_identifier;
extern bool is_tsql_rowversion_or_timestamp_datatype(Oid oid);

/*****************************************
 * 			Catalog Hooks
 *****************************************/
IsExtendedCatalogHookType PrevIsExtendedCatalogHook = NULL;
static bool PlTsqlMatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
							     bool include_out_arguments, int pronargs,
							     int **argnumbers, List **defaults);
static bool match_pltsql_func_call(HeapTuple proctup, int nargs, List *argnames,
								   bool include_out_arguments, int **argnumbers,
								   List **defaults, bool expand_defaults, bool expand_variadic,
								   bool *use_defaults, bool *any_special,
								   bool *variadic, Oid *va_elem_type);

/*****************************************
 * 			Analyzer Hooks
 *****************************************/
static void set_output_clause_transformation_info(bool enabled);
static bool get_output_clause_transformation_info(void);
static Node *output_update_self_join_transformation(ParseState *pstate, UpdateStmt *stmt, CmdType command);
static void handle_returning_qualifiers(CmdType command, List *returningList, ParseState *pstate);
static void check_insert_row(List *icolumns, List *exprList, Oid relid);
static void pltsql_post_transform_column_definition(ParseState *pstate, RangeVar* relation, ColumnDef *column, List **alist);
static void pltsql_post_transform_table_definition(ParseState *pstate, RangeVar* relation, char *relname, List **alist);
static void pre_transform_target_entry(ResTarget *res, ParseState *pstate, ParseExprKind exprKind);
static bool tle_name_comparison(const char *tlename, const char *identifier);
static void resolve_target_list_unknowns(ParseState *pstate, List *targetlist);
static inline bool is_identifier_char(char c);
static int find_attr_by_name_from_relation(Relation rd, const char *attname, bool sysColOK);
static void modify_insert_stmt(InsertStmt *stmt, Oid relid);

/*****************************************
 * 			Commands Hooks
 *****************************************/
static int find_attr_by_name_from_column_def_list(const char *attributeName, List *schema);
static void pltsql_drop_func_default_positions(Oid objectId);

/*****************************************
 * 			Utility Hooks
 *****************************************/
static void pltsql_report_proc_not_found_error(List *names, List *argnames, int nargs, ParseState *pstate, int location, bool proc_call);
extern PLtsql_execstate *get_outermost_tsql_estate(int *nestlevel);
static void pltsql_store_view_definition(const char *queryString, ObjectAddress address);
static void pltsql_drop_view_definition(Oid objectId);
static void preserve_view_constraints_from_base_table(ColumnDef  *col, Oid tableOid, AttrNumber colId);
static bool pltsql_detect_numeric_overflow(int weight, int dscale, int first_block, int numeric_base);
static void insert_pltsql_function_defaults(HeapTuple func_tuple, List *defaults, Node **argarray);
static int print_pltsql_function_arguments(StringInfo buf, HeapTuple proctup, bool print_table_args, bool print_defaults);
/*****************************************
 * 			Executor Hooks
 *****************************************/
static void pltsql_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pltsql_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once);
static void pltsql_ExecutorFinish(QueryDesc *queryDesc);
static void pltsql_ExecutorEnd(QueryDesc *queryDesc);

static bool plsql_TriggerRecursiveCheck(ResultRelInfo *resultRelInfo);

/*****************************************
 * 			Replication Hooks
 *****************************************/
static void logicalrep_modify_slot(Relation rel, EState *estate, TupleTableSlot *slot);

/*****************************************
 * 			Object Access Hook
 *****************************************/
static object_access_hook_type prev_object_access_hook = NULL;
static void bbf_object_access_hook(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg);
static void revoke_func_permission_from_public(Oid objectId);
static char *gen_func_arg_list(Oid objectId);

/* Save hook values in case of unload */
static core_yylex_hook_type prev_core_yylex_hook = NULL;
static pre_transform_returning_hook_type prev_pre_transform_returning_hook = NULL;
static pre_transform_insert_hook_type prev_pre_transform_insert_hook = NULL;
static post_transform_insert_row_hook_type prev_post_transform_insert_row_hook = NULL;
static pre_transform_target_entry_hook_type prev_pre_transform_target_entry_hook = NULL;
static tle_name_comparison_hook_type prev_tle_name_comparison_hook = NULL;
static resolve_target_list_unknowns_hook_type prev_resolve_target_list_unknowns_hook = NULL;
static find_attr_by_name_from_column_def_list_hook_type prev_find_attr_by_name_from_column_def_list_hook = NULL;
static find_attr_by_name_from_relation_hook_type prev_find_attr_by_name_from_relation_hook = NULL;
static report_proc_not_found_error_hook_type prev_report_proc_not_found_error_hook = NULL;
static store_view_definition_hook_type prev_store_view_definition_hook = NULL;
static logicalrep_modify_slot_hook_type prev_logicalrep_modify_slot_hook = NULL;
static is_tsql_rowversion_or_timestamp_datatype_hook_type prev_is_tsql_rowversion_or_timestamp_datatype_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static inherit_view_constraints_from_table_hook_type prev_inherit_view_constraints_from_table = NULL;
static detect_numeric_overflow_hook_type prev_detect_numeric_overflow_hook = NULL;
static match_pltsql_func_call_hook_type prev_match_pltsql_func_call_hook = NULL;
static insert_pltsql_function_defaults_hook_type prev_insert_pltsql_function_defaults_hook = NULL;
static print_pltsql_function_arguments_hook_type prev_print_pltsql_function_arguments_hook = NULL;
/*****************************************
 * 			Install / Uninstall
 *****************************************/
void
InstallExtendedHooks(void)
{
	if (IsExtendedCatalogHook)
		PrevIsExtendedCatalogHook = IsExtendedCatalogHook;
	IsExtendedCatalogHook = &IsPLtsqlExtendedCatalog;

	prev_object_access_hook = object_access_hook;
	object_access_hook = bbf_object_access_hook;

	prev_core_yylex_hook = core_yylex_hook;
	core_yylex_hook = pgtsql_core_yylex;

	get_output_clause_status_hook = get_output_clause_transformation_info;
	pre_output_clause_transformation_hook = output_update_self_join_transformation;

	prev_pre_transform_returning_hook = pre_transform_returning_hook;
	pre_transform_returning_hook = handle_returning_qualifiers;

	prev_pre_transform_insert_hook = pre_transform_insert_hook;
	pre_transform_insert_hook = modify_insert_stmt;

	prev_post_transform_insert_row_hook = post_transform_insert_row_hook;
	post_transform_insert_row_hook = check_insert_row;

	post_transform_column_definition_hook = pltsql_post_transform_column_definition;

	post_transform_table_definition_hook = pltsql_post_transform_table_definition;

	prev_pre_transform_target_entry_hook = pre_transform_target_entry_hook;
	pre_transform_target_entry_hook = pre_transform_target_entry;

	prev_tle_name_comparison_hook = tle_name_comparison_hook;
	tle_name_comparison_hook = tle_name_comparison;

	prev_resolve_target_list_unknowns_hook = resolve_target_list_unknowns_hook;
	resolve_target_list_unknowns_hook = resolve_target_list_unknowns;

	prev_find_attr_by_name_from_column_def_list_hook = find_attr_by_name_from_column_def_list_hook;
	find_attr_by_name_from_column_def_list_hook = find_attr_by_name_from_column_def_list;

	prev_find_attr_by_name_from_relation_hook = find_attr_by_name_from_relation_hook;
	find_attr_by_name_from_relation_hook = find_attr_by_name_from_relation;

	prev_report_proc_not_found_error_hook = report_proc_not_found_error_hook;
	report_proc_not_found_error_hook = pltsql_report_proc_not_found_error;

	prev_store_view_definition_hook = store_view_definition_hook;
	store_view_definition_hook = pltsql_store_view_definition;

	prev_logicalrep_modify_slot_hook = logicalrep_modify_slot_hook;
	logicalrep_modify_slot_hook = logicalrep_modify_slot;

	prev_is_tsql_rowversion_or_timestamp_datatype_hook = is_tsql_rowversion_or_timestamp_datatype_hook;
	is_tsql_rowversion_or_timestamp_datatype_hook = is_tsql_rowversion_or_timestamp_datatype;

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pltsql_ExecutorStart;

	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pltsql_ExecutorRun;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pltsql_ExecutorFinish;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pltsql_ExecutorEnd;

	prev_inherit_view_constraints_from_table = inherit_view_constraints_from_table_hook;
	inherit_view_constraints_from_table_hook = preserve_view_constraints_from_base_table;
	TriggerRecuresiveCheck_hook = plsql_TriggerRecursiveCheck;

	prev_detect_numeric_overflow_hook = detect_numeric_overflow_hook;
	detect_numeric_overflow_hook = pltsql_detect_numeric_overflow;

	prev_match_pltsql_func_call_hook = match_pltsql_func_call_hook;
	match_pltsql_func_call_hook = match_pltsql_func_call;

	prev_insert_pltsql_function_defaults_hook = insert_pltsql_function_defaults_hook;
	insert_pltsql_function_defaults_hook = insert_pltsql_function_defaults;

	prev_print_pltsql_function_arguments_hook = print_pltsql_function_arguments_hook;
	print_pltsql_function_arguments_hook = print_pltsql_function_arguments;
}

void
UninstallExtendedHooks(void)
{
	IsExtendedCatalogHook = PrevIsExtendedCatalogHook;

	object_access_hook = prev_object_access_hook;

	core_yylex_hook = prev_core_yylex_hook;
	get_output_clause_status_hook = NULL;
	pre_output_clause_transformation_hook = NULL;
	pre_transform_returning_hook = prev_pre_transform_returning_hook;
	pre_transform_insert_hook = prev_pre_transform_insert_hook ;
	post_transform_insert_row_hook = prev_post_transform_insert_row_hook;
	post_transform_column_definition_hook = NULL;
	post_transform_table_definition_hook = NULL;
	pre_transform_target_entry_hook = prev_pre_transform_target_entry_hook;
	tle_name_comparison_hook = prev_tle_name_comparison_hook;
	resolve_target_list_unknowns_hook = prev_resolve_target_list_unknowns_hook;
	find_attr_by_name_from_column_def_list_hook = prev_find_attr_by_name_from_column_def_list_hook;
	find_attr_by_name_from_relation_hook = prev_find_attr_by_name_from_relation_hook;
	report_proc_not_found_error_hook = prev_report_proc_not_found_error_hook;
	store_view_definition_hook = prev_store_view_definition_hook;
	logicalrep_modify_slot_hook = prev_logicalrep_modify_slot_hook;
	is_tsql_rowversion_or_timestamp_datatype_hook = prev_is_tsql_rowversion_or_timestamp_datatype_hook;
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	ExecutorEnd_hook = prev_ExecutorEnd;
	inherit_view_constraints_from_table_hook = prev_inherit_view_constraints_from_table;
	detect_numeric_overflow_hook = prev_detect_numeric_overflow_hook;
	match_pltsql_func_call_hook = prev_match_pltsql_func_call_hook;
	insert_pltsql_function_defaults_hook = prev_insert_pltsql_function_defaults_hook;
	print_pltsql_function_arguments_hook = prev_print_pltsql_function_arguments_hook;
}

/*****************************************
 * 			Hook Functions
 *****************************************/

static void
pltsql_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	int ef = pltsql_explain_only ? EXEC_FLAG_EXPLAIN_ONLY : eflags;
	if (is_explain_analyze_mode())
	{
		if (pltsql_explain_timing)
			queryDesc->instrument_options |= INSTRUMENT_TIMER;
		else
			queryDesc->instrument_options |= INSTRUMENT_ROWS;
		if (pltsql_explain_buffers)
			queryDesc->instrument_options |= INSTRUMENT_BUFFERS;
		if (pltsql_explain_wal)
			queryDesc->instrument_options |= INSTRUMENT_WAL;
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, ef);
	else
		standard_ExecutorStart(queryDesc, ef);

	if (is_explain_analyze_mode() && !queryDesc->totaltime)
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun. Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
		MemoryContextSwitchTo(oldcxt);
	}
}

static void
pltsql_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
{
	if (pltsql_explain_only)
	{
		EState	   *estate;
		CmdType		operation;
		DestReceiver *dest;
		MemoryContext oldcontext;

		Assert(queryDesc != NULL);
		estate = queryDesc->estate;
		Assert(estate != NULL);

		oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
		operation = queryDesc->operation;
		dest = queryDesc->dest;

		/*
		 * startup tuple receiver, if we will be emitting tuples
		 */
		estate->es_processed = 0;
		if (operation == CMD_SELECT || queryDesc->plannedstmt->hasReturning)
		{
			dest->rStartup(dest, operation, queryDesc->tupDesc);
			dest->rShutdown(dest);
		}

		MemoryContextSwitchTo(oldcontext);
		return;
	}

	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
}

static void
pltsql_ExecutorFinish(QueryDesc *queryDesc)
{
	if (pltsql_explain_only)
		return;

	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

static void
pltsql_ExecutorEnd(QueryDesc *queryDesc)
{
	append_explain_info(queryDesc, NULL);

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/**
 * @brief 
 *  the function will depend on PLtsql_execstate to find whether 
 *  the trigger is called before on this query stack, so that's why we 
 *  have to add a hook into Postgres code to callback into babel code,
 *  since we need to get access to PLtsql_execstate to iterate the 
 *  stack triggers
 *  
 *  return true if it's a recursive call of trigger
 *  return false if it's not
 * 
 * @param resultRelInfo 
 * @return true 
 * @return false 
 */
static bool
plsql_TriggerRecursiveCheck(ResultRelInfo *resultRelInfo)
{
	if (resultRelInfo->ri_TrigDesc == NULL)
		return false;
	if (pltsql_recursive_triggers)
		return false;
	int i;
	PLExecStateCallStack * cur;
	PLtsql_execstate *estate;
	cur = exec_state_call_stack;
	while (cur != NULL){
		estate = cur->estate;
		if (estate->trigdata != NULL && estate->trigdata->tg_trigger != NULL
		&& resultRelInfo->ri_TrigDesc != NULL 
		&& (resultRelInfo->ri_TrigDesc->trig_insert_instead_statement
		|| resultRelInfo->ri_TrigDesc->trig_delete_instead_statement 
		|| resultRelInfo->ri_TrigDesc->trig_update_instead_statement)){
			for (i = 0; i<resultRelInfo->ri_TrigDesc->numtriggers; ++i){
				Trigger    *trigger = &resultRelInfo->ri_TrigDesc->triggers[i];
				if (trigger->tgoid == estate->trigdata->tg_trigger->tgoid){
					return true;
				}
			}
		}
		cur = cur->next;
	}
	return false;
}

static Node *
output_update_self_join_transformation(ParseState *pstate, UpdateStmt *stmt, CmdType command)
{
	Node	    *qual = NULL, *pre_transform_qual = NULL;
	RangeVar	*from_table = NULL;
	ColumnRef	*l_expr, *r_expr;
	A_Expr 		*where_ctid = NULL;
	Node 		*where_clone = NULL;

	/* 
	* Invoke transformWhereClause() to check for ambiguities in column name
	* of the original query before self-join transformation.
	*/
	where_clone = copyObject(stmt->whereClause);
	pre_transform_qual = transformWhereClause(pstate, stmt->whereClause,
							EXPR_KIND_WHERE, "WHERE");

	if (sql_dialect != SQL_DIALECT_TSQL)
		return pre_transform_qual;

	if (get_output_clause_transformation_info())
	{
		/* Unset the OUTPUT clause info variable to prevent unintended side-effects */
		set_output_clause_transformation_info(false);

		/* Add target table with deleted alias to the from clause */
		from_table = makeRangeVar(NULL, stmt->relation->relname, -1);
		from_table->alias = makeAlias("deleted", NIL);
		stmt->fromClause = list_make1(from_table);
		transformFromClause(pstate, stmt->fromClause);

		/* Create the self-join condition based on ctid */
		l_expr = makeNode(ColumnRef);
		l_expr->fields = list_make2(makeString(stmt->relation->relname), makeString("ctid"));
		l_expr->location = -1;

		r_expr = makeNode(ColumnRef);
		r_expr->fields = list_make2(makeString("deleted"), makeString("ctid"));
		r_expr->location = -1;
		where_ctid = makeA_Expr(AEXPR_OP, list_make1(makeString("=")), (Node*) l_expr, (Node*) r_expr, -1);

		/* Add the self-join condition to the where clause */
		if (where_clone)
		{
			BoolExpr *self_join_condition;
			self_join_condition = (BoolExpr*) makeBoolExpr(AND_EXPR, list_make2(where_clone, where_ctid), -1);
			stmt->whereClause = (Node*) self_join_condition;
		}
		else
			stmt->whereClause = (Node*) where_ctid;

		/* Set the OUTPUT clause info variable to be used in transformColumnRef() */
		set_output_clause_transformation_info(true);

		/*
		* We let transformWhereClause() be called before the invokation of this hook
		* to handle ambiguity errors. If there are any ambiguous references in the
		* query an error is thrown. At this point, we have cleared that check and 
		* know that there are no ambiguities. Therefore, we can go ahead with the
		* where clause transformation without worrying about ambiguous references.
		*/
		qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

		/* Unset the OUTPUT clause info variable because we do not need it anymore */
		set_output_clause_transformation_info(false);
	}
	else
		qual = pre_transform_qual;

	handle_returning_qualifiers(command, stmt->returningList, pstate);
	return qual;
}

static void
set_output_clause_transformation_info(bool enabled)
{
	output_update_transformation = enabled;
}

static bool
get_output_clause_transformation_info(void)
{
	return output_update_transformation;
}

static void
handle_returning_qualifiers(CmdType command, List *returningList, ParseState *pstate)
{
	ListCell   			*o_target, *expr;
	Node	   			*field1;
	char	   			*qualifier = NULL; 
	ParseNamespaceItem  *nsitem = NULL;
	int 				levels_up;
	bool 				inserted = false, deleted = false;
	List				*queue = NIL;

	if (prev_pre_transform_returning_hook)
		prev_pre_transform_returning_hook(command, returningList, pstate);

	if (sql_dialect != SQL_DIALECT_TSQL || returningList == NIL)
		return;

	if (command == CMD_INSERT || command == CMD_DELETE)
	{
		foreach(o_target, returningList)
		{
			ResTarget  *res = (ResTarget *) lfirst(o_target);
			queue = NIL;
			queue = list_make1(res->val);

			foreach(expr, queue)
			{
				Node *node = (Node *) lfirst(expr);
				if (IsA(node, ColumnRef))
				{
					ColumnRef  *cref = (ColumnRef *) node;

					if (command == CMD_INSERT)
						nsitem = refnameNamespaceItem(pstate, NULL, "inserted",
													  cref->location,
													  &levels_up);

					if (command == CMD_DELETE)
						nsitem = refnameNamespaceItem(pstate, NULL, "deleted",
													  cref->location,
													  &levels_up);

					if (nsitem)
						return;

					if (list_length(cref->fields) == 2)
					{
						field1 = (Node *) linitial(cref->fields);
						qualifier = strVal(field1);

						if ((command == CMD_INSERT && !strcmp(qualifier, "inserted"))
							|| (command == CMD_DELETE && !strcmp(qualifier, "deleted")))
							cref->fields = list_delete_first(cref->fields);
					}
				}
				else if(IsA(node, A_Expr))
				{
					A_Expr  *a_expr = (A_Expr *) node;

					if (a_expr->lexpr)
						queue = lappend(queue, a_expr->lexpr);
					if (a_expr->rexpr)
						queue = lappend(queue, a_expr->rexpr);
				}
				else if(IsA(node, FuncCall))
				{
					FuncCall *func_call = (FuncCall*) node;
					if (func_call->args)
						queue = list_concat(queue, func_call->args);
				}
			}
		}
	}
	else if (command == CMD_UPDATE)
	{
		foreach(o_target, returningList)
		{
			ResTarget  *res = (ResTarget *) lfirst(o_target);
			queue = NIL;
			queue = list_make1(res->val);

			foreach(expr, queue)
			{
				Node *node = (Node *) lfirst(expr);

				if (IsA(node, ColumnRef))
				{
					/*
					* Checks for RTEs could have been performed outside of the loop
					* but we need to perform them inside the loop so that we can pass
					* cref->location to refnameRangeTblEntry() and keep error messages
					* correct.
					*/
					ColumnRef  *cref = (ColumnRef *) node;
					nsitem = refnameNamespaceItem(pstate, NULL, "inserted",
												  cref->location,
												  &levels_up);

					if (nsitem)
						inserted = true;

					nsitem = refnameNamespaceItem(pstate, NULL, "deleted",
												  cref->location,
												  &levels_up);
					if (nsitem)
						deleted = true;

					if (inserted && deleted)
						break;
				}
				else if(IsA(node, A_Expr))
				{
					A_Expr  *a_expr = (A_Expr *) node;

					if (a_expr->lexpr)
						queue = lappend(queue, a_expr->lexpr);
					if (a_expr->rexpr)
						queue = lappend(queue, a_expr->rexpr);
				}
				else if(IsA(node, FuncCall))
				{
					FuncCall *func_call = (FuncCall*) node;
					if (func_call->args)
						queue = list_concat(queue, func_call->args);
				}
			}
		}
		foreach(o_target, returningList)
		{
			ResTarget  *res = (ResTarget *) lfirst(o_target);
			queue = NIL;
			queue = list_make1(res->val);

			foreach(expr, queue)
			{
				Node *node = (Node *) lfirst(expr);

				if (IsA(node, ColumnRef))
				{
					ColumnRef  *cref = (ColumnRef *) node;
					if (list_length(cref->fields) == 2)
					{
						field1 = (Node *) linitial(cref->fields);
						qualifier = strVal(field1);

						if ((!inserted && !strcmp(qualifier, "inserted")) || (!deleted && !strcmp(qualifier, "deleted")))
						{
							if (update_delete_target_alias)
								/*
								 * If target relation is specified by an alias
								 * in FROM clause, we should use the alias
								 * instead of the relation name, because
								 * otherwise "inserted" will still show the
								 * previous value.
								 */
								linitial(cref->fields) = makeString(update_delete_target_alias);
							else
								linitial(cref->fields) = makeString(RelationGetRelationName(pstate->p_target_relation));
						}
					}
				}
				else if(IsA(node, A_Expr))
				{
					A_Expr  *a_expr = (A_Expr *) node;

					if (a_expr->lexpr)
						queue = lappend(queue, a_expr->lexpr);
					if (a_expr->rexpr)
						queue = lappend(queue, a_expr->rexpr);
				}
				else if(IsA(node, FuncCall))
				{
					FuncCall *func_call = (FuncCall*) node;
					if (func_call->args)
						queue = list_concat(queue, func_call->args);
				}
			}
		}
	}
}

static void
check_insert_row(List *icolumns, List *exprList, Oid relid)
{
	/* Do not allow more target columns than expressions */
	if (exprList != NIL && list_length(exprList) < list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("Number of given values does not match target table definition")));
}

char*
extract_identifier(const char *start)
{
	/*
	 * We will extract original identifier from source query string. 'start' is a char potiner to start of identifier, which is provided from caller based on location of each token.
	 * The remaning task is to find the end position of identifier. For this, we will mimic the lexer rule.
	 * This includes plain identifier (and un-reserved keyword) as well as delimited by double-quote and squared bracket (T-SQL).
	 *
	 * Please note that, this function assumes that identifier is already valid. (otherwise, syntax error should be already thrown).
	 */

	bool dq = false;
	bool sqb = false;
	int i = 0;
	char *original_name = NULL;
	bool valid = false;
	bool found_escaped_in_dq = false;

	/* check identifier is delimited */
	Assert(start);
	if (start[0] == '"')
		dq = true;
	else if (start[0] == '[')
		sqb = true;
	++i; /* advance cursor by one. As it is already a valid identiifer, its length should be greater than 1 */

	/* valid identifier cannot be longer than 258 (2*128+2) bytes. SQL server allows up to 128 bascially. And escape character can take additional one byte for each character in worst case. And additional 2 byes for delimiter */
	while (i < 258)
	{
		char c = start[i];

		if (!dq && !sqb) /* normal case */
		{
			/* please see {tsql_ident_cont} in scan-tsql-decl.l */
			valid = is_identifier_char(c);
			if (!valid)
			{
				original_name = palloc(i + 1);
				memcpy(original_name, start, i);
				original_name[i] = '\0';
				return original_name;
			}
		}
		else if (dq)
		{
			/* please see xdinside in scan.l */
			valid = (c != '"');
			if (!valid && start[i+1] == '"') /* escaped */
			{
				++i; ++i; /* advance two characters */
				found_escaped_in_dq = true;
				continue;
			}

			if (!valid)
			{
				if (!found_escaped_in_dq)
				{
					/* no escaped character. copy whole string at once */
					original_name = palloc(i); /* exclude first/last double quote */
					memcpy(original_name, start + 1, i -1);
					original_name[i - 1] = '\0';
					return original_name;
				}
				else
				{
					/* there is escaped character. copy one by one to handle escaped character */
					int rcur = 1; /* read-cursor */
					int wcur = 0; /* write-cursor */
					original_name = palloc(i); /* exclude first/last double quote */
					for (; rcur<i; ++rcur, ++wcur)
					{
						original_name[wcur] = start[rcur];
						if (start[rcur] == '"')
							++rcur; /* skip next character */
					}
					original_name[wcur] = '\0';
					return original_name;
				}
			}
		}
		else if (sqb)
		{
			/* please see xbrinside in scan-tsql-decl.l */
			valid = (c != ']');
			if (!valid)
			{
				original_name = palloc(i); /* exclude first/last square bracket */
				memcpy(original_name, start + 1, i -1);
				original_name[i - 1] = '\0';
				return original_name;
			}
		}

		++i;
	}

	return NULL;
}

extern const char *ATTOPTION_BBF_ORIGINAL_NAME;

static void
pltsql_post_transform_column_definition(ParseState *pstate, RangeVar* relation, ColumnDef *column, List **alist)
{
	/* add "ALTER TABLE ALTER COLUMN SET (bbf_original_name=<original_name>)" to alist so that original_name will be stored in pg_attribute.attoptions */

	AlterTableStmt *stmt;
	AlterTableCmd *cmd;
	// To get original column name, utilize location of ColumnDef and query string.
	const char* column_name_start = pstate->p_sourcetext + column->location;
	char* original_name = extract_identifier(column_name_start);
	if (original_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("can't extract original column name")));


	cmd = makeNode(AlterTableCmd);
	cmd->subtype = AT_SetOptions;
	cmd->name = column->colname;
	cmd->def = (Node *) list_make1(makeDefElem(pstrdup(ATTOPTION_BBF_ORIGINAL_NAME), (Node *) makeString(pstrdup(original_name)), column->location));
	cmd->behavior = DROP_RESTRICT;
	cmd->missing_ok = false;

	stmt = makeNode(AlterTableStmt);
	stmt->relation = relation;
	stmt->cmds = NIL;
	stmt->objtype = OBJECT_TABLE;
	stmt->cmds = lappend(stmt->cmds, cmd);

	(*alist) = lappend(*alist, stmt);
}

extern const char *ATTOPTION_BBF_ORIGINAL_TABLE_NAME;

static void
pltsql_post_transform_table_definition(ParseState *pstate, RangeVar* relation, char *relname, List **alist)
{
	/* add "ALTER TABLE SET (bbf_original_table_name=<original_name>)" to alist so that original_name will be stored in pg_class.reloptions */
	AlterTableStmt *stmt;
	AlterTableCmd *cmd;

	/* To get original column name, utilize location of relation and query string. */
	char *table_name_start, *original_name, *temp;

	table_name_start = pstate->p_sourcetext + relation->location;

	/*
	 * Could be the case that the fully qualified name is included,
	 * so just find the text after '.' in the identifier.
	 * We need to be careful as there can be '.' in the table name
	 * itself, so we will break the loop if current string matches
	 * with actual relname.
	 */
	temp = strpbrk(table_name_start, ". ");
	while (temp && temp[0] != ' ' &&
		strncasecmp(relname, table_name_start, strlen(relname)) != 0 &&
		strncasecmp(relname, table_name_start + 1, strlen(relname)) != 0) /* match after skipping delimiter */
	{
		temp += 1;
		table_name_start = temp;
		temp = strpbrk(table_name_start, ". ");
	}

	original_name = extract_identifier(table_name_start);
	if (original_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("can't extract original table name")));

	/* Only store if there's a difference, and if the difference is only in capitalization */
	if (strncmp(relname, original_name, strlen(relname)) == 0 || strncasecmp(relname, original_name, strlen(relname)) != 0)
	{
		return;
	}

	cmd = makeNode(AlterTableCmd);
	cmd->subtype = AT_SetRelOptions;
	cmd->def = (Node *) list_make1(makeDefElem(pstrdup(ATTOPTION_BBF_ORIGINAL_TABLE_NAME), (Node *) makeString(pstrdup(original_name)), -1));
	cmd->behavior = DROP_RESTRICT;
	cmd->missing_ok = false;

	stmt = makeNode(AlterTableStmt);
	stmt->relation = relation;
	stmt->cmds = NIL;
	stmt->objtype = OBJECT_TABLE;
	stmt->cmds = lappend(stmt->cmds, cmd);

	(*alist) = lappend(*alist, stmt);
}

static void
resolve_target_list_unknowns(ParseState *pstate, List *targetlist)
{
	ListCell   *l;

	if (sql_dialect != SQL_DIALECT_TSQL)
		return;

	foreach(l, targetlist)
	{
		Const 		*con;
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Oid			restype = exprType((Node *) tle->expr);

		if (restype != UNKNOWNOID)
			continue;

		if (!IsA(tle->expr, Const))
			continue;

		con = (Const *) tle->expr;
		if (con->constisnull)
		{
			/* In T-SQL, NULL const (without explicit datatype) should be resolved as INT4 */
			tle->expr = (Expr *) coerce_type(pstate, (Node *) con,
												restype, INT4OID, -1,
												COERCION_IMPLICIT,
												COERCE_IMPLICIT_CAST,
												-1);
		}
		else
		{
			Oid sys_nspoid = get_namespace_oid("sys", false);
			Oid sys_varchartypoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
													CStringGetDatum("varchar"), ObjectIdGetDatum(sys_nspoid));
			tle->expr = (Expr *) coerce_type(pstate, (Node *) con,
												restype, sys_varchartypoid, -1,
												COERCION_IMPLICIT,
												COERCE_IMPLICIT_CAST,
												-1);
		}
	}
}

static inline bool
is_identifier_char(char c)
{
	/* please see {tsql_ident_cont} in scan-tsql-decl.l */
	bool valid = ((c >= 'A' && c <= 'Z') ||
		      (c >= 'a' && c <= 'z') ||
		      (c >= 0200 && c <= 0377) ||
		      (c >= '0' && c <= '9') ||
		      c == '_' || c == '$' || c == '#');

	return valid;
}

static int
find_attr_by_name_from_column_def_list(const char *attributeName, List *schema)
{
	char *attrname = downcase_identifier(attributeName, strlen(attributeName), false, false);
	int attrlen = strlen(attrname);
	int i = 1;
	ListCell *s;

	foreach(s, schema)
	{
		ColumnDef  *def = lfirst(s);

		if (strlen(def->colname) == attrlen)
		{
			char *defname;

			if (strcmp(attributeName, def->colname) == 0) // compare with original strings
				return i;

			defname = downcase_identifier(def->colname, strlen(def->colname), false, false);
			if (strncmp(attrname, defname, attrlen) == 0) // compare with downcased strings
				return i;
		}
		i++;
	}

	return InvalidAttrNumber;
}

/* specialAttNum()
 *
 * Check attribute name to see if it is "special", e.g. "xmin".
 * - thomas 2000-02-07
 *
 * Note: this only discovers whether the name could be a system attribute.
 * Caller needs to ensure that it really is an attribute of the rel.
 */
static int
specialAttNum(const char *attname)
{
	const FormData_pg_attribute *sysatt;

	sysatt = SystemAttributeByName(attname);
	if (sysatt != NULL)
		return sysatt->attnum;
	return InvalidAttrNumber;
}

static int
find_attr_by_name_from_relation(Relation rd, const char *attname, bool sysColOK)
{
	int i;

	for (i = 0; i < RelationGetNumberOfAttributes(rd); i++)
	{
		Form_pg_attribute att = TupleDescAttr(rd->rd_att, i);
		const char *origname = NameStr(att->attname);
		int rdattlen = strlen(origname);
		const char *rdattname;

		if (strlen(attname) == rdattlen && !att->attisdropped)
		{
			if (namestrcmp(&(att->attname), attname) == 0) // compare with original strings
				return i + 1;

			/*
			 * Currently, we don't have any cases where attname needs to be downcased
			 * If exists, we have to take a deeper look
			 * whether the downcasing is needed here or gram.y
			 */
			rdattname = downcase_identifier(origname, rdattlen, false, false);
			if (strcmp(rdattname, attname) == 0) // compare with downcased strings
				return i + 1;
		}
	}

	if (sysColOK)
	{
		if ((i = specialAttNum(attname)) != InvalidAttrNumber)
			return i;
	}

	/* on failure */
	return InvalidAttrNumber;
}

static void
pre_transform_target_entry(ResTarget *res, ParseState *pstate,
									   ParseExprKind exprKind)
{
	if (prev_pre_transform_target_entry_hook)
		(*prev_pre_transform_target_entry_hook) (res, pstate, exprKind);

	/* In the TSQL dialect construct an AS clause for each target list
	 * item that is a column using the capitalization from the sourcetext.
	 */
	if (sql_dialect == SQL_DIALECT_TSQL &&
		exprKind == EXPR_KIND_SELECT_TARGET)
	{
		int alias_len = 0;
		const char *colname_start;
		const char *identifier_name = NULL;

		if (res->name == NULL && res->location != -1 &&
			IsA(res->val, ColumnRef))
		{
			ColumnRef *cref = (ColumnRef *) res->val;

			/* If no alias is specified on a ColumnRef, then
			 * get the length of the name from the ColumnRef and
			 * copy the column name from the sourcetext
			 */
			if (list_length(cref->fields) == 1 &&
				IsA(linitial(cref->fields), String))
			{
				identifier_name = strVal(linitial(cref->fields));
				alias_len = strlen(identifier_name);
				colname_start = pstate->p_sourcetext + res->location;
			}
		}
		else if (res->name != NULL && res->name_location != -1)
		{
			identifier_name = res->name;
			alias_len = strlen(res->name);
			colname_start = pstate->p_sourcetext + res->name_location;
		}

		if (alias_len > 0)
		{
			char *alias = palloc0(alias_len + 1);
			bool dq = *colname_start == '"';
			bool sqb = *colname_start == '[';
			bool sq = *colname_start == '\'';
			int a = 0;
			const char *colname_end;
			bool closing_quote_reached = false;

			if (dq || sqb || sq)
			{
				colname_start++;
			}

			if (dq || sq)
			{

				for (colname_end = colname_start; a < alias_len; colname_end++)
				{
					if (dq && *colname_end == '"')
					{
						if ((*(++colname_end) != '"'))
						{
							closing_quote_reached = true;
							break; /* end of dbl-quoted identifier */
						}
					}
					else if (sq && *colname_end == '\'')
					{
						if ((*(++colname_end) != '\''))
						{
							closing_quote_reached = true;
							break; /* end of single-quoted identifier */
						}
					}

					alias[a++] = *colname_end;
				}

				// Assert(a == alias_len);
			}
			else
			{
				colname_end = colname_start + alias_len;
				memcpy(alias, colname_start, alias_len);
			}

			/* If the end of the string is a uniquifier, then copy
			 * the uniquifier into the last 32 characters of
			 * the alias
			 */
			if (alias_len == NAMEDATALEN-1 &&
			    (((sq || dq) && !closing_quote_reached) ||
			     is_identifier_char(*colname_end)))

			{
				memcpy(alias+(NAMEDATALEN-1)-32,
				       identifier_name+(NAMEDATALEN-1)-32,
				       32);
				alias[NAMEDATALEN] = '\0';
			}

			res->name = alias;
		}
	}
}

static bool
tle_name_comparison(const char *tlename, const char *identifier)
{
	if (sql_dialect == SQL_DIALECT_TSQL)
	{
		int tlelen = strlen(tlename);

		if (tlelen != strlen(identifier))
			return false;

		if (pltsql_case_insensitive_identifiers)
			return (0 == strcmp(downcase_identifier(tlename, tlelen, false, false),
					    downcase_identifier(identifier, tlelen, false, false)));
		else
			return (0 == strcmp(tlename, identifier));
	}
	else if (prev_tle_name_comparison_hook)
		return (*prev_tle_name_comparison_hook) (tlename, identifier);
	else
		return (0 == strcmp(tlename, identifier));
}

/* Generate similar error message with SQL Server when function/procedure is not found if possible. */
void
pltsql_report_proc_not_found_error(List *names, List *given_argnames, int nargs, ParseState *pstate, int location, bool proc_call)
{
	FuncCandidateList candidates = NULL, current_candidate = NULL;
	int max_nargs = -1;
	int min_nargs = INT_MAX;
	int ncandidates = 0;
	bool found = false;
	const char *obj_type = proc_call ? "procedure" : "function";

	candidates = FuncnameGetCandidates(names, -1, NIL, false, false, false, true); /* search all possible candidate regardless of the # of arguments */
	if (candidates == NULL)
		return; /* no candidates at all. let backend handle the proc-not-found error */

	for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next)
	{
		if (current_candidate->nargs == nargs) /* Found the proc/func having the same number of arguments. */
			found = true;
		
		ncandidates++;
		min_nargs = (current_candidate->nargs < min_nargs) ? current_candidate->nargs : min_nargs;
		max_nargs = (current_candidate->nargs > max_nargs) ? current_candidate->nargs : max_nargs;
	}

	if (max_nargs == -1 || min_nargs == INT_MAX) /* Unexpected number of arguments, let PG backend handle the error message */
		return;

	if (ncandidates > 1) /* More than one candidates exist, throwing an error message with possible number of arguments */
	{
		const char *arg_str = (max_nargs < 2) ? "argument" : "arguments";

		/* Found the proc/func having the same number of arguments. possibly data-type mistmatch. */
		if (found)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("The %s %s is found but cannot be used. Possibly due to datatype mismatch and implicit casting is not allowed.", obj_type, NameListToString(names))),
					 parser_errposition(pstate, location));
		}

		if (max_nargs == min_nargs)
		{
			if (max_nargs == 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("%s %s has too many arguments specified.", obj_type, NameListToString(names))),
						 parser_errposition(pstate, location));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("The %s %s requires %d %s", NameListToString(names), obj_type, max_nargs, arg_str)),
						 parser_errposition(pstate, location));
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("The %s %s requires %d to %d %s", NameListToString(names), obj_type, min_nargs, max_nargs, arg_str)),
					 parser_errposition(pstate, location));
		}
	}
	else /* Only one candidate exists, */
	{
		HeapTuple tup;
		bool isnull;

		tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(candidates->oid));
		if (HeapTupleIsValid(tup))
		{
			(void) SysCacheGetAttr(PROCOID, tup,
									Anum_pg_proc_proargnames,
									&isnull);
			
			if(!isnull)
			{
				Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(tup);
				HeapTuple bbffunctuple;
				int pronargs = procform->pronargs;
				int first_arg_with_default = pronargs - procform->pronargdefaults;
				int pronallargs;
				int ap;
				int pp;
				int numposargs = nargs - list_length(given_argnames);
				Oid *p_argtypes;
				char **p_argnames;
				char *p_argmodes;
				char *first_unknown_argname = NULL;
				bool arggiven[FUNC_MAX_ARGS];
				bool default_positions_available = false;
				List *default_positions = NIL;
				ListCell *lc;
				char *langname = get_language_name(procform->prolang, true);

				if (nargs > pronargs) /* Too many parameters provided. */
				{
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("%s %s has too many arguments specified.",obj_type, NameListToString(names))),
							 parser_errposition(pstate, location));
				}
				
				pronallargs = get_func_arg_info(tup,
												&p_argtypes,
												&p_argnames,
												&p_argmodes);
				memset(arggiven, false, pronargs * sizeof(bool));

				/* there are numposargs positional args before the named args */
				for (ap = 0; ap < numposargs; ap++)
					arggiven[ap] = true;

				foreach(lc, given_argnames)
				{
					char *argname = (char *) lfirst(lc);
					bool match_found;
					int i;

					pp = 0;
					match_found = false;
					for (i = 0; i < pronallargs; i++)
					{
						/* consider only input parameters */
						if (p_argmodes &&
						    (p_argmodes[i] != FUNC_PARAM_IN &&
							 p_argmodes[i] != FUNC_PARAM_INOUT &&
							 p_argmodes[i] != FUNC_PARAM_VARIADIC))
							continue;
						if (p_argnames[i] && strcmp(p_argnames[i], argname) == 0)
						{
							arggiven[pp] = true;
							match_found = true;
							break;
						}
						/* increase pp only for input parameters */
						pp++;
					}
					/* Store first unknown parameter name. */
					if (!match_found && first_unknown_argname == NULL)
						first_unknown_argname = argname;
				}
				
				if (langname && pg_strcasecmp("pltsql", langname) == 0 && nargs < pronargs)
				{
					bbffunctuple = get_bbf_function_tuple_from_proctuple(tup);

					if (HeapTupleIsValid(bbffunctuple))
					{
						Datum	arg_default_positions;
						char	*str;

						/* Fetch default positions */
						arg_default_positions = SysCacheGetAttr(PROCNSPSIGNATURE,
																bbffunctuple,
																Anum_bbf_function_ext_default_positions,
																&isnull);

						if (!isnull)
						{
							str = TextDatumGetCString(arg_default_positions);
							default_positions = castNode(List, stringToNode(str));
							lc = list_head(default_positions);
							default_positions_available = true;
							pfree(str);
						}
						else
							ReleaseSysCache(bbffunctuple);
					}
				}

				/* Traverse arggiven list to check if a non-default parameter is not supplied. */
				for (pp = numposargs; pp < pronargs; pp++)
				{
					if (arggiven[pp])
						continue;

					/*
					 * If the positions of default arguments are available then we need
					 * special handling. Look into default_positions list to find out
					 * the default expression for pp'th argument.
					 */
					if (default_positions_available)
					{
						bool has_default = false;
						
						/*
						 * Iterate over argdefaults list to find out the default expression
						 * for current argument.
						 */
						while (lc != NULL)
						{
							int position = intVal((Node *) lfirst(lc));

							if (position == pp)
							{
								has_default = true;
								lc = lnext(default_positions, lc);
								break;
							}
							else if (position > pp)
								break;
							lc = lnext(default_positions, lc);
						}

						if (!has_default)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("%s %s expects parameter \"%s\", which was not supplied.", obj_type, NameListToString(names), p_argnames[pp])),
									 parser_errposition(pstate, location));
					}
					else if (pp < first_arg_with_default)
					{
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_FUNCTION),
								 errmsg("%s %s expects parameter \"%s\", which was not supplied.", obj_type, NameListToString(names), p_argnames[pp])),
								 parser_errposition(pstate, location));
					}
				}
				/* Default arguments are also supplied but parameter name is unknown. */
				if(first_unknown_argname)
				{
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("\"%s\" is not an parameter for %s %s.", first_unknown_argname, obj_type, NameListToString(names))),
							 parser_errposition(pstate, location));
				}
				/* Still no issue with the arguments provided, possibly data-type mistmatch. */
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("The %s %s is found but cannot be used. Possibly due to datatype mismatch and implicit casting is not allowed.", obj_type, NameListToString(names))),
						 parser_errposition(pstate, location));
				
				if (default_positions_available)
				{
					ReleaseSysCache(bbffunctuple);
				}
				pfree(langname);
			}
			else if(nargs > 0) /* proargnames is NULL. Procedure/function has no parameters but arguments are specified. */
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("%s %s has no parameters and arguments were supplied.", obj_type, NameListToString(names))),
						 parser_errposition(pstate, location));
			}	
		}
		ReleaseSysCache(tup);
	}
}

/*
 * Perform necessary modification on a slot which is going to be inserted/updated
 * in the target relation by logical replication worker.
 */
static void
logicalrep_modify_slot(Relation rel, EState *estate, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int			attnum;
	ExprContext *econtext;

	econtext = GetPerTupleExprContext(estate);

	for (attnum = 0; attnum < desc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, attnum);

		if (attr->attisdropped || attr->attgenerated)
			continue;

		/*
		 * If it is rowversion/timestamp column, then re-evaluate the column default
		 * and replace the slot with this new value.
		 */
		if (is_tsql_rowversion_or_timestamp_datatype(attr->atttypid))
		{
			Expr *defexpr;
			ExprState *def;

			defexpr = (Expr *) build_column_default(rel, attnum + 1);

			if (defexpr != NULL)
			{
				/* Run the expression through planner */
				defexpr = expression_planner(defexpr);
				def = ExecInitExpr(defexpr, NULL);
				slot->tts_values[attnum] = ExecEvalExpr(def, econtext, &slot->tts_isnull[attnum]);
				/*
				* No need to check for other columns since we can only
				* have one rowversion/timestamp column in a table.
				*/
				break;
			}
		}
	}
}

static void
bbf_object_access_hook(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg)
{
	/* Call previous hook if exists */
	if (prev_object_access_hook)
		(*prev_object_access_hook) (access, classId, objectId, subId, arg);

	if (access == OAT_DROP && classId == RelationRelationId)
		pltsql_drop_view_definition(objectId);

	if (access == OAT_DROP && classId == ProcedureRelationId)
		pltsql_drop_func_default_positions(objectId);

	if (sql_dialect != SQL_DIALECT_TSQL)
		return;

	if (access == OAT_DROP && classId == AuthIdRelationId)
		drop_bbf_roles(access, classId, objectId, subId, arg);

	if (access == OAT_POST_CREATE && classId == ProcedureRelationId)
		revoke_func_permission_from_public(objectId);
}

static void revoke_func_permission_from_public(Oid objectId)
{
	const char 	*query;
	List		*res;
	GrantStmt   *revoke;
	PlannedStmt *wrapper;
	const char	*obj_name;
	Oid			phy_sch_oid;
	const char	*phy_sch_name;
	const char  *arg_list;
	char		kind;

	/* TSQL specific behavior */
	if (sql_dialect != SQL_DIALECT_TSQL)
		return;

	/* Advance command counter so new tuple can be seen by validator */
	CommandCounterIncrement();

	/* get properties */
	obj_name = get_func_name(objectId);
	phy_sch_oid = get_func_namespace(objectId);
	phy_sch_name = get_namespace_name(phy_sch_oid);
	kind = get_func_prokind(objectId);
	arg_list = gen_func_arg_list(objectId);

	/* prepare subcommand */
	if (kind == PROKIND_PROCEDURE)
		query = psprintf("REVOKE ALL ON PROCEDURE [%s].[%s](%s) FROM PUBLIC", phy_sch_name, obj_name, arg_list);
	else
		query = psprintf("REVOKE ALL ON FUNCTION [%s].[%s](%s) FROM PUBLIC", phy_sch_name, obj_name, arg_list);

	res = raw_parser(query, RAW_PARSE_DEFAULT);

	if (list_length(res) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("Expected 1 statement but get %d statements after parsing",
						list_length(res))));

	revoke = (GrantStmt *) parsetree_nth_stmt(res, 0);

	wrapper = makeNode(PlannedStmt);
	wrapper->commandType = CMD_UTILITY;
	wrapper->canSetTag = false;
	wrapper->utilityStmt = (Node *) revoke;
	wrapper->stmt_location = 0;
	wrapper->stmt_len = 0;

	ProcessUtility(wrapper,
				   query,
				   false,
				   PROCESS_UTILITY_SUBCOMMAND,
				   NULL,
				   NULL,
				   None_Receiver,
				   NULL);

	/* Command Counter will be increased by validator */
}

static char *gen_func_arg_list(Oid objectId)
{
	Oid *argtypes;
	int nargs = 0;
	StringInfoData arg_list;
	initStringInfo(&arg_list);

	get_func_signature(objectId, &argtypes, &nargs);

	for (int i = 0; i < nargs; i++)
	{
		Oid typoid = argtypes[i];
		char *nsp_name;
		char *type_name;
		HeapTuple   typeTuple;

		typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));

		if (!HeapTupleIsValid(typeTuple))
			return NULL;

		type_name = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(typeTuple))->typname));
		nsp_name = get_namespace_name(((Form_pg_type) GETSTRUCT(typeTuple))->typnamespace);
		ReleaseSysCache(typeTuple);

		appendStringInfoString(&arg_list, nsp_name);
		appendStringInfoString(&arg_list, ".");
		appendStringInfoString(&arg_list, type_name);
		if (i < nargs -1)
			appendStringInfoString(&arg_list, ", ");
	}

	return arg_list.data;
}

/*
* This function adds column names to the insert target relation in rewritten
* CTE for OUTPUT INTO clause. 
*/
static void 
modify_insert_stmt(InsertStmt *stmt, Oid relid)
{
	Relation	pg_attribute;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tuple;
	List		*insert_col_list = NIL, *temp_col_list;
		
	if (prev_pre_transform_insert_hook)
		(*prev_pre_transform_insert_hook) (stmt, relid);
	
	if (sql_dialect != SQL_DIALECT_TSQL)
		return;

	if (!output_into_insert_transformation)
		return;

	if (stmt->cols != NIL)
		return;

	/* Get column names from the relation */
	ScanKeyInit(&scankey,
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	pg_attribute = table_open(AttributeRelationId, AccessShareLock);
	
	scan = systable_beginscan(pg_attribute, AttributeRelidNumIndexId, true,
							  NULL, 1, &scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ResTarget *col = makeNode(ResTarget);
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(tuple);
		temp_col_list = NIL;

		if (att->attnum > 0)
		{
			col->name = NameStr(att->attname);
			col->indirection = NIL;
			col->val = NULL;
			col->location = 1;
			col->name_location = 1;
			temp_col_list = list_make1(col);
			insert_col_list = list_concat(insert_col_list, temp_col_list);
		}
	}
	stmt->cols = insert_col_list;
	systable_endscan(scan);
	table_close(pg_attribute, AccessShareLock);

}

/*
 * Stores view object's TSQL definition to bbf_view_def catalog
 * Note: It won't store view info if view is created in TSQL dialect from PG
 * endpoint as dbid will be NULL in that case.
 */
static void
pltsql_store_view_definition(const char *queryString, ObjectAddress address)
{
	/* Store TSQL definition */
	Relation	bbf_view_def_rel;
	TupleDesc	bbf_view_def_rel_dsc;
	Datum		new_record[BBF_VIEW_DEF_NUM_COLS];
	bool		new_record_nulls[BBF_VIEW_DEF_NUM_COLS];
	HeapTuple	tuple, reltup;
	Form_pg_class	form_reltup;
	int16		dbid;
	uint64		flag_values = 0, flag_validity = 0;
	char		*physical_schemaname, *logical_schemaname;

	if (sql_dialect != SQL_DIALECT_TSQL)
		return;

	/* Skip if it is for sysdatabases while creating logical database */
	if (strcmp("(CREATE LOGICAL DATABASE )", queryString) == 0)
		return;

	/* Fetch the object details from Relation */
	reltup = SearchSysCache1(RELOID, ObjectIdGetDatum(address.objectId));
	form_reltup = (Form_pg_class) GETSTRUCT(reltup);

	physical_schemaname = get_namespace_name(form_reltup->relnamespace);
	if (physical_schemaname == NULL)
	{
		elog(ERROR,
				"Could not find physical schemaname for %u",
				 form_reltup->relnamespace);
	}

	/*
	 * Do not store definition/data in case of sys, information_schema_tsql and
	 * other shared schemas.
	 */
	if (is_shared_schema(physical_schemaname))
	{
		pfree(physical_schemaname);
		ReleaseSysCache(reltup);
		return;
	}

	dbid = get_dbid_from_physical_schema_name(physical_schemaname, true);
	logical_schemaname = get_logical_schema_name(physical_schemaname, true);
	if(!DbidIsValid(dbid) || logical_schemaname == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("Could not find dbid or logical schema for this physical schema '%s'." \
				"CREATE VIEW from non-babelfish schema/db is not allowed in TSQL dialect.", physical_schemaname)));
	}

	bbf_view_def_rel = table_open(get_bbf_view_def_oid(), RowExclusiveLock);
	bbf_view_def_rel_dsc = RelationGetDescr(bbf_view_def_rel);

	MemSet(new_record_nulls, false, sizeof(new_record_nulls));

	/*
	 * To use particular flag bit to store certain flag, Set corresponding bit
	 * in flag_validity which tracks currently supported flag bits and then
	 * set/unset flag_values bit according to flag settings.
	 * Used !Transform_null_equals instead of pltsql_ansi_nulls because NULL is
	 * being inserted in catalog if it is used.
	 * Currently, Only two flags are supported.
	 */
	flag_validity |= BBF_VIEW_DEF_FLAG_IS_ANSI_NULLS_ON;
	if (!Transform_null_equals)
		flag_values |= BBF_VIEW_DEF_FLAG_IS_ANSI_NULLS_ON;
	flag_validity |= BBF_VIEW_DEF_FLAG_USES_QUOTED_IDENTIFIER;
	if (pltsql_quoted_identifier)
		flag_values |= BBF_VIEW_DEF_FLAG_USES_QUOTED_IDENTIFIER;

	new_record[0] = Int16GetDatum(dbid);
	new_record[1] = CStringGetTextDatum(logical_schemaname);
	new_record[2] = CStringGetTextDatum(NameStr(form_reltup->relname));
	new_record[3] = CStringGetTextDatum(queryString);
	new_record[4] = UInt64GetDatum(flag_validity);
	new_record[5] = UInt64GetDatum(flag_values);

	tuple = heap_form_tuple(bbf_view_def_rel_dsc,
							new_record, new_record_nulls);

	CatalogTupleInsert(bbf_view_def_rel, tuple);

	pfree(physical_schemaname);
	pfree(logical_schemaname);
	ReleaseSysCache(reltup);
	heap_freetuple(tuple);
	table_close(bbf_view_def_rel, RowExclusiveLock);
}

/*
 * Drops view object's TSQL definition from bbf_view_def catalog
 */
static void
pltsql_drop_view_definition(Oid objectId)
{
	Relation	bbf_view_def_rel;
	HeapTuple	reltuple, scantup;
	Form_pg_class	form;
	int16		dbid;
	char		*physical_schemaname, *logical_schemaname, *objectname;

	/* return if it is not a view */
	reltuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(reltuple))
		return;					/* concurrently dropped */
	form = (Form_pg_class) GETSTRUCT(reltuple);
	if (form->relkind != RELKIND_VIEW)
	{
		ReleaseSysCache(reltuple);
		return;
	}

	physical_schemaname = get_namespace_name(form->relnamespace);
	if (physical_schemaname == NULL)
	{
		elog(ERROR,
				"Could not find physical schemaname for %u",
				 form->relnamespace);
	}
	dbid = get_dbid_from_physical_schema_name(physical_schemaname, true);
	logical_schemaname = get_logical_schema_name(physical_schemaname, true);
	objectname = NameStr(form->relname);

	/*
	 * If any of these entries are NULL then there
	 * must not be any entry in catalog
	 */
	if (!DbidIsValid(dbid) || logical_schemaname == NULL || objectname == NULL)
	{
		pfree(physical_schemaname);
		if (logical_schemaname)
			pfree(logical_schemaname);
		ReleaseSysCache(reltuple);
		return;
	}

	/* Fetch the relation */
	bbf_view_def_rel = table_open(get_bbf_view_def_oid(), RowExclusiveLock);

	scantup = search_bbf_view_def(bbf_view_def_rel, dbid, logical_schemaname, objectname);

	if (HeapTupleIsValid(scantup))
	{
		CatalogTupleDelete(bbf_view_def_rel,
						   &scantup->t_self);
		heap_freetuple(scantup);
	}

	pfree(physical_schemaname);
	pfree(logical_schemaname);
	ReleaseSysCache(reltuple);
	table_close(bbf_view_def_rel, RowExclusiveLock);
}

static void
preserve_view_constraints_from_base_table(ColumnDef  *col, Oid tableOid, AttrNumber colId)
{
	/*
	 * In TSQL Dialect Preserve the constraints only for the internal view
	 * created by sp_describe_first_result_set procedure.
	 */
	if (sp_describe_first_result_set_inprogress && sql_dialect == SQL_DIALECT_TSQL)
	{
		HeapTuple	  tp;
		Form_pg_attribute att_tup;

		tp = SearchSysCache2(ATTNUM, 
					ObjectIdGetDatum(tableOid),
					Int16GetDatum(colId));

		if (HeapTupleIsValid(tp))
		{
			att_tup = (Form_pg_attribute) GETSTRUCT(tp);
			col->is_not_null = att_tup->attnotnull;
			col->identity 	 = att_tup->attidentity;
			col->generated 	 = att_tup->attgenerated;
			ReleaseSysCache(tp);
		}
	}
}

/*
 * detect_numeric_overflow() -
 * 	Calculate exact number of digits of any numeric data and report if numeric overflow occurs
 */
bool
pltsql_detect_numeric_overflow(int weight, int dscale, int first_block, int numeric_base)
{
	if (sql_dialect != SQL_DIALECT_TSQL)
		return false;

	int partially_filled_numeric_block = 0;
	int total_digit_count = 0;

	total_digit_count = (dscale == 0) ? (weight * numeric_base) :
					    ((weight + 1) * numeric_base);
	/*
	 * calculating exact #digits in the first partially filled numeric block, if any)
	 * Ex. - in 12345.12345 var is of type struct NumericVar; first_block = var->digits[0]= 1,
	 * var->digits[1] = 2345, var->digits[2] = 1234,
	 * var->digits[3] = 5000; numeric_base = 4, var->ndigits = #numeric blocks i.e., 4,
	 * var->weight = 1, var->dscale = 5
	 */
	partially_filled_numeric_block = first_block;

	/*
	 * check if the first numeric block is partially filled
	 * If yes, add those digit count
	 * Else if fully filled, Ignore as those digits are already added to total_digit_count
	 */
	if (partially_filled_numeric_block < pow(10, numeric_base - 1))
		total_digit_count += (partially_filled_numeric_block > 0) ?
				     log10(partially_filled_numeric_block) + 1 : 1;

	/*
	 * calculating exact #digits in last block if decimal point exists
	 * If dscale is an exact multiple of numeric_base, last block is not partially filled,
	 * then, ignore as those digits are already added to total_digit_count
	 * Else, add the remainder digits
	 */
	if (dscale > 0)
		total_digit_count += (dscale % numeric_base);

	return (total_digit_count > TDS_NUMERIC_MAX_PRECISION);
}

/*
 * Stores argument positions of default values of a PL/tsql function to bbf_function_ext catalog
 */
void
pltsql_store_func_default_positions(ObjectAddress address, List *parameters)
{
	Relation	bbf_function_ext_rel;
	TupleDesc	bbf_function_ext_rel_dsc;
	Datum		new_record[BBF_FUNCTION_EXT_NUM_COLS];
	bool		new_record_nulls[BBF_FUNCTION_EXT_NUM_COLS];
	bool		new_record_replaces[BBF_FUNCTION_EXT_NUM_COLS];
	HeapTuple	tuple, proctup, oldtup;
	Form_pg_proc	form_proctup;
	char		*physical_schemaname;
	char		*func_signature;
	List		*default_positions = NIL;
	ListCell	*x;
	int			idx;

	/* Fetch the object details from function */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(address.objectId));
	/* Disallow extended catalog lookup during restore */
	if (!HeapTupleIsValid(proctup) || babelfish_dump_restore)
		return;
	form_proctup = (Form_pg_proc) GETSTRUCT(proctup);

	if (!is_pltsql_language_oid(form_proctup->prolang))
	{
		ReleaseSysCache(proctup);
		return;
	}

	physical_schemaname = get_namespace_name(form_proctup->pronamespace);
	if (physical_schemaname == NULL)
	{
		elog(ERROR,
				"Could not find physical schemaname for %u",
				 form_proctup->pronamespace);
	}

	/*
	 * Do not store data in case of sys, information_schema_tsql and
	 * other shared schemas.
	 */
	if (is_shared_schema(physical_schemaname))
	{
		pfree(physical_schemaname);
		ReleaseSysCache(proctup);
		return;
	}

	func_signature = get_pltsql_function_signature(NameStr(form_proctup->proname),
												   form_proctup->pronargs,
												   form_proctup->proargtypes.values);

	idx = 0;
	foreach(x, parameters)
	{
		FunctionParameter *fp = (FunctionParameter *) lfirst(x);

		if (fp->defexpr)
		{
			default_positions = lappend(default_positions, (Node *) makeInteger(idx));
		}
		idx++;
	}

	if (!OidIsValid(get_bbf_function_ext_idx_oid()))
		return;

	bbf_function_ext_rel = table_open(get_bbf_function_ext_oid(), RowExclusiveLock);
	bbf_function_ext_rel_dsc = RelationGetDescr(bbf_function_ext_rel);

	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	MemSet(new_record_replaces, false, sizeof(new_record_replaces));

	new_record[Anum_bbf_function_ext_nspname -1] = CStringGetDatum(physical_schemaname);
	new_record[Anum_bbf_function_ext_funcname -1] = NameGetDatum(&form_proctup->proname);
	new_record_nulls[Anum_bbf_function_ext_orig_name -1] = true; /* TODO: Fill users' original input name */
	new_record[Anum_bbf_function_ext_funcsignature - 1] = CStringGetTextDatum(func_signature);
	if (default_positions != NIL)
		new_record[Anum_bbf_function_ext_default_positions - 1] = CStringGetTextDatum(nodeToString(default_positions));
	else
		new_record_nulls[Anum_bbf_function_ext_default_positions - 1] = true;
	new_record_replaces[Anum_bbf_function_ext_default_positions - 1] = true;

	oldtup = get_bbf_function_tuple_from_proctuple(proctup);

	if (HeapTupleIsValid(oldtup))
	{
		tuple = heap_modify_tuple(oldtup, bbf_function_ext_rel_dsc,
								  new_record, new_record_nulls,
								  new_record_replaces);
		CatalogTupleUpdate(bbf_function_ext_rel, &tuple->t_self, tuple);

		ReleaseSysCache(oldtup);
	}
	else
	{
		ObjectAddress index;
		tuple = heap_form_tuple(bbf_function_ext_rel_dsc,
								new_record, new_record_nulls);

		CatalogTupleInsert(bbf_function_ext_rel, tuple);

		/*
		 * Add function's dependency on catalog table's index so
		 * that table gets restored before function during MVU.
		 */
		index.classId = IndexRelationId;
		index.objectId = get_bbf_function_ext_idx_oid();
		index.objectSubId = 0;
		recordDependencyOn(&address, &index, DEPENDENCY_NORMAL);
	}

	pfree(physical_schemaname);
	pfree(func_signature);
	ReleaseSysCache(proctup);
	heap_freetuple(tuple);
	table_close(bbf_function_ext_rel, RowExclusiveLock);
}

/*
 * Drops argument positions of default values of a PL/tsql function from bbf_function_ext catalog
 */
static void
pltsql_drop_func_default_positions(Oid objectId)
{
	HeapTuple	 proctuple, bbffunctuple;

	/* return if it is not a PL/tsql function */
	proctuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(proctuple))
		return;					/* concurrently dropped */

	bbffunctuple = get_bbf_function_tuple_from_proctuple(proctuple);

	if (HeapTupleIsValid(bbffunctuple))
	{
		Relation	 bbf_function_ext_rel;

		/* Fetch the relation */
		bbf_function_ext_rel = table_open(get_bbf_function_ext_oid(), RowExclusiveLock);

		CatalogTupleDelete(bbf_function_ext_rel,
						   &bbffunctuple->t_self);
		table_close(bbf_function_ext_rel, RowExclusiveLock);
		ReleaseSysCache(bbffunctuple);
	}

	ReleaseSysCache(proctuple);
}

static bool
match_pltsql_func_call(HeapTuple proctup, int nargs, List *argnames,
					   bool include_out_arguments, int **argnumbers,
					   List **defaults, bool expand_defaults, bool expand_variadic,
					   bool *use_defaults, bool *any_special,
					   bool *variadic, Oid *va_elem_type)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
	int			pronargs = procform->pronargs;

	if (argnames != NIL)
	{
		/*
		 * Call uses named or mixed notation
		 *
		 * Named or mixed notation can match a variadic function only if
		 * expand_variadic is off; otherwise there is no way to match the
		 * presumed-nameless parameters expanded from the variadic array.
		 */
		if (OidIsValid(procform->provariadic) && expand_variadic)
			return false;
		*va_elem_type = InvalidOid;
		*variadic = false;

		/*
		 * Check argument count.
		 */
		Assert(nargs >= 0); /* -1 not supported with argnames */

		if (pronargs > nargs && expand_defaults)
		{
			/* Ignore if not enough default expressions */
			if (nargs + procform->pronargdefaults < pronargs)
				return false;
			*use_defaults = true;
		}
		else
			*use_defaults = false;

		/* Ignore if it doesn't match requested argument count */
		if (pronargs != nargs && !(*use_defaults))
			return false;

		/* Check for argument name match, generate positional mapping */
		if (!PlTsqlMatchNamedCall(proctup, nargs, argnames,
								  include_out_arguments, pronargs,
								  argnumbers, defaults))
			return false;

		/* Named argument matching is always "special" */
		*any_special = true;
	}
	else
	{
		/*
		 * Call uses positional notation
		 *
		 * Check if function is variadic, and get variadic element type if
		 * so.  If expand_variadic is false, we should just ignore
		 * variadic-ness.
		 */
		if (pronargs <= nargs && expand_variadic)
		{
			*va_elem_type = procform->provariadic;
			*variadic = OidIsValid(*va_elem_type);
			*any_special |= *variadic;
		}
		else
		{
			*va_elem_type = InvalidOid;
			*variadic = false;
		}

		/*
		 * Check if function can match by using parameter defaults.
		 */
		if (pronargs > nargs && expand_defaults)
		{
			/* Ignore if not enough default expressions */
			if (nargs + procform->pronargdefaults < pronargs)
				return false;
			*use_defaults = true;
			*any_special = true;
		}
		else
			*use_defaults = false;

		/* Ignore if it doesn't match requested argument count */
		if (nargs >= 0 && pronargs != nargs && !(*variadic) && !(*use_defaults))
			return false;

		/*
		 * If call uses all positional arguments, then validate if all
		 * the remaining arguments have defaults.
		 */
		if (*use_defaults)
		{
			HeapTuple	bbffunctuple = get_bbf_function_tuple_from_proctuple(proctup);

			if (HeapTupleIsValid(bbffunctuple))
			{
				Datum	 arg_default_positions;
				bool	 isnull;

				/* Fetch default positions */
				arg_default_positions = SysCacheGetAttr(PROCNSPSIGNATURE,
														bbffunctuple,
														Anum_bbf_function_ext_default_positions,
														&isnull);

				if (!isnull)
				{
					char	 *str;
					List	 *default_positions = NIL;
					ListCell *def_idx = NULL;
					int		 idx = nargs;

					str = TextDatumGetCString(arg_default_positions);
					default_positions = castNode(List, stringToNode(str));
					pfree(str);

					foreach(def_idx, default_positions)
					{
						int position = intVal((Node *) lfirst(def_idx));

						if (position == idx)
							idx++;
					}

					/* we could not find defaults for some arguments. */
					if (idx < pronargs)
					{
						ReleaseSysCache(bbffunctuple);
						return false;
					}
				}

				ReleaseSysCache(bbffunctuple);
			}
		}
	}

	return true;
}

/*
 * PlTsqlMatchNamedCall
 *		Given a pg_proc heap tuple of a PL/tsql function and a call's list of
 *		argument names, check whether the function could match the call.
 *
 * The call could match if all supplied argument names are accepted by
 * the function, in positions after the last positional argument, and there
 * are defaults for all unsupplied arguments.
 *
 * Most of the implementation of this function has been taken from backend's
 * MatchNamedCall function (see catalog/namespace.c) but it has been modified
 * to use babelfish_function_ext catalog to get the default positions, if
 * available.
 *
 * On match, return true and fill *argnumbers with a palloc'd array showing
 * the mapping from call argument positions to actual function argument
 * numbers. Defaulted arguments are included in this map, at positions
 * after the last supplied argument.
 * Additionally if default positions are available in babelfish_function_ext
 * catalog then fill *defaults with list of default expression nodes for
 * unsupplied arguments.
 */
static bool
PlTsqlMatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
					 bool include_out_arguments, int pronargs,
					 int **argnumbers, List **defaults)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
	int			 numposargs = nargs - list_length(argnames);
	int			 pronallargs;
	Oid		    *p_argtypes;
	char	   **p_argnames;
	char	    *p_argmodes;
	bool		 arggiven[FUNC_MAX_ARGS];
	bool		 isnull;
	int			 ap;				/* call args position */
	int			 pp;				/* proargs position */
	ListCell    *lc;

	Assert(argnames != NIL);
	Assert(numposargs >= 0);
	Assert(nargs <= pronargs);

	/* Ignore this function if its proargnames is null */
	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proargnames,
						   &isnull);
	if (isnull)
		return false;

	/* OK, let's extract the argument names and types */
	pronallargs = get_func_arg_info(proctup,
									&p_argtypes, &p_argnames, &p_argmodes);
	Assert(p_argnames != NULL);

	Assert(include_out_arguments ? (pronargs == pronallargs) : (pronargs <= pronallargs));

	/* initialize state for matching */
	*argnumbers = (int *) palloc(pronargs * sizeof(int));
	memset(arggiven, false, pronargs * sizeof(bool));

	/* there are numposargs positional args before the named args */
	for (ap = 0; ap < numposargs; ap++)
	{
		(*argnumbers)[ap] = ap;
		arggiven[ap] = true;
	}

	/* now examine the named args */
	foreach(lc, argnames)
	{
		char	   *argname = (char *) lfirst(lc);
		bool		found;
		int			i;

		pp = 0;
		found = false;
		for (i = 0; i < pronallargs; i++)
		{
			/* consider only input params, except with include_out_arguments */
			if (!include_out_arguments &&
				p_argmodes &&
				(p_argmodes[i] != FUNC_PARAM_IN &&
				 p_argmodes[i] != FUNC_PARAM_INOUT &&
				 p_argmodes[i] != FUNC_PARAM_VARIADIC))
				continue;
			if (p_argnames[i] && strcmp(p_argnames[i], argname) == 0)
			{
				/* fail if argname matches a positional argument */
				if (arggiven[pp])
					return false;
				arggiven[pp] = true;
				(*argnumbers)[ap] = pp;
				found = true;
				break;
			}
			/* increase pp only for considered parameters */
			pp++;
		}
		/* if name isn't in proargnames, fail */
		if (!found)
			return false;
		ap++;
	}

	Assert(ap == nargs);		/* processed all actual parameters */

	/* Check for default arguments */
	*defaults = NIL;
	if (nargs < pronargs)
	{
		int			first_arg_with_default = pronargs - procform->pronargdefaults;
		HeapTuple	bbffunctuple = get_bbf_function_tuple_from_proctuple(proctup);
		List		*argdefaults = NIL,
					*default_positions = NIL;
		bool		default_positions_available = false;
		ListCell	*def_item = NULL,
					*def_idx = NULL;
		bool		match_found = true;

		if (HeapTupleIsValid(bbffunctuple))
		{
			Datum		proargdefaults;
			Datum		arg_default_positions;

			/* Fetch argument defaults */
			proargdefaults = SysCacheGetAttr(PROCOID, proctup,
											Anum_pg_proc_proargdefaults,
											&isnull);

			if (!isnull)
			{
				char *str;

				str = TextDatumGetCString(proargdefaults);
				argdefaults = castNode(List, stringToNode(str));
				def_item = list_head(argdefaults);
				pfree(str);
			}

			/* Fetch default positions */
			arg_default_positions = SysCacheGetAttr(PROCNSPSIGNATURE,
													bbffunctuple,
													Anum_bbf_function_ext_default_positions,
													&isnull);

			if (!isnull)
			{
				char *str;

				str = TextDatumGetCString(arg_default_positions);
				default_positions = castNode(List, stringToNode(str));
				def_idx = list_head(default_positions);
				default_positions_available = true;
				pfree(str);
			}
			else
				ReleaseSysCache(bbffunctuple);
		}

		for (pp = numposargs; pp < pronargs; pp++)
		{
			if (arggiven[pp])
				continue;

			/*
			 * If the positions of default arguments are available then we need
			 * special handling. Look into default_positions list to find out
			 * the default expression for pp'th argument.
			 */
			if (default_positions_available)
			{
				bool has_default = false;

				/*
				 * Iterate over argdefaults list to find out the default expression
				 * for current argument.
				 */
				while (def_item != NULL && def_idx != NULL)
				{
					int position = intVal((Node *) lfirst(def_idx));

					if (position == pp)
					{
						has_default = true;
						*defaults = lappend(*defaults, lfirst(def_item));
						def_item = lnext(argdefaults, def_item);
						def_idx = lnext(default_positions, def_idx);
						break;
					}
					else if (position > pp)
						break;
					def_item = lnext(argdefaults, def_item);
					def_idx = lnext(default_positions, def_idx);
				}

				if (!has_default)
				{
					match_found = false;
					break;
				}
				(*argnumbers)[ap++] = pp;
				continue;
			}
			/* fail if arg not given and no default available */
			else if (pp < first_arg_with_default)
			{
				match_found = false;
				break;
			}
			(*argnumbers)[ap++] = pp;
		}

		if (default_positions_available)
			ReleaseSysCache(bbffunctuple);

		if (!match_found)
			return false;
	}

	Assert(ap == pronargs);		/* processed all function parameters */

	return true;
}

/*
 * insert_pltsql_function_defaults
 *		Given a pg_proc heap tuple of a PL/tsql function and list of defaults,
 *		fill missing arguments in *argarray with default expressions.
 *
 * If given PL/tsql function has default positions available from babelfish_function_ext
 * catalog then use them to fill *argarray, otherwise fallback to PG's way to
 * fill only last few arguments with defaults.
 */
static void
insert_pltsql_function_defaults(HeapTuple func_tuple, List *defaults, Node **argarray)
{
	HeapTuple	bbffunctuple;

	bbffunctuple = get_bbf_function_tuple_from_proctuple(func_tuple);

	if (HeapTupleIsValid(bbffunctuple))
	{
		Datum				  arg_default_positions;
		bool				  isnull;

		/* Fetch default positions */
		arg_default_positions = SysCacheGetAttr(PROCNSPSIGNATURE,
												bbffunctuple,
												Anum_bbf_function_ext_default_positions,
												&isnull);

		if (!isnull)
		{
			char				  *str;
			List				  *default_positions = NIL;
			ListCell			  *def_idx = NULL,
								  *def_item = NULL;

			str = TextDatumGetCString(arg_default_positions);
			default_positions = castNode(List, stringToNode(str));
			pfree(str);

			forboth(def_idx, default_positions, def_item, defaults)
			{
				int position = intVal((Node *) lfirst(def_idx));

				if (argarray[position] == NULL)
					argarray[position] = (Node *) lfirst(def_item);
			}
		}

		ReleaseSysCache(bbffunctuple);
	}
	else
	{
		Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
		int			 i;
		ListCell	*lc = NIL;

		i = funcform->pronargs - funcform->pronargdefaults;
		foreach(lc, defaults)
		{
			if (argarray[i] == NULL)
				argarray[i] = (Node *) lfirst(lc);
			i++;
		}
	}
}

/*
 * Same as backend's print_function_arguments (see ruleutils.c)
 * but only for PL/tsql functions. If given function has default
 * positions available from babelfish_function_ext catalog then use
 * them to print default arguments.
 */
static int
print_pltsql_function_arguments(StringInfo buf, HeapTuple proctup,
								bool print_table_args, bool print_defaults)
{
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proctup);
	HeapTuple	bbffunctuple;
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			insertorderbyat = -1;
	int			argsprinted;
	int			inputargno;
	bool		isnull;
	bool		default_positions_available = false;
	int			nlackdefaults;
	List	   *argdefaults = NIL;
	List	   *defaultpositions = NIL;
	ListCell   *nextargdefault = NULL;
	ListCell   *nextdefaultposition = NULL;
	int			i;

	numargs = get_func_arg_info(proctup,
								&argtypes, &argnames, &argmodes);

	nlackdefaults = numargs;
	if (print_defaults && proc->pronargdefaults > 0)
	{
		Datum		proargdefaults;

		proargdefaults = SysCacheGetAttr(PROCOID, proctup,
										 Anum_pg_proc_proargdefaults,
										 &isnull);
		if (!isnull)
		{
			char	   *str;

			str = TextDatumGetCString(proargdefaults);
			argdefaults = castNode(List, stringToNode(str));
			pfree(str);
			nextargdefault = list_head(argdefaults);
			/* nlackdefaults counts only *input* arguments lacking defaults */
			nlackdefaults = proc->pronargs - list_length(argdefaults);
		}
	}

	bbffunctuple = get_bbf_function_tuple_from_proctuple(proctup);

	if (HeapTupleIsValid(bbffunctuple))
	{
		Datum		arg_default_positions;
		char	   *str;

		/* Fetch default positions */
		arg_default_positions = SysCacheGetAttr(PROCNSPSIGNATURE,
												bbffunctuple,
												Anum_bbf_function_ext_default_positions,
												&isnull);

		if (!isnull)
		{
			str = TextDatumGetCString(arg_default_positions);
			defaultpositions = castNode(List, stringToNode(str));
			nextdefaultposition = list_head(defaultpositions);
			default_positions_available = true;
			pfree(str);
		}
		else
			ReleaseSysCache(bbffunctuple);
	}

	/* Check for special treatment of ordered-set aggregates */
	if (proc->prokind == PROKIND_AGGREGATE)
	{
		HeapTuple	aggtup;
		Form_pg_aggregate agg;

		aggtup = SearchSysCache1(AGGFNOID, proc->oid);
		if (!HeapTupleIsValid(aggtup))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 proc->oid);
		agg = (Form_pg_aggregate) GETSTRUCT(aggtup);
		if (AGGKIND_IS_ORDERED_SET(agg->aggkind))
			insertorderbyat = agg->aggnumdirectargs;
		ReleaseSysCache(aggtup);
	}

	argsprinted = 0;
	inputargno = 0;
	for (i = 0; i < numargs; i++)
	{
		Oid			argtype = argtypes[i];
		char	   *argname = argnames ? argnames[i] : NULL;
		char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;
		const char *modename;
		bool		isinput;

		switch (argmode)
		{
			case PROARGMODE_IN:

				/*
				 * For procedures, explicitly mark all argument modes, so as
				 * to avoid ambiguity with the SQL syntax for DROP PROCEDURE.
				 */
				if (proc->prokind == PROKIND_PROCEDURE)
					modename = "IN ";
				else
					modename = "";
				isinput = true;
				break;
			case PROARGMODE_INOUT:
				modename = "INOUT ";
				isinput = true;
				break;
			case PROARGMODE_OUT:
				modename = "OUT ";
				isinput = false;
				break;
			case PROARGMODE_VARIADIC:
				modename = "VARIADIC ";
				isinput = true;
				break;
			case PROARGMODE_TABLE:
				modename = "";
				isinput = false;
				break;
			default:
				elog(ERROR, "invalid parameter mode '%c'", argmode);
				modename = NULL;	/* keep compiler quiet */
				isinput = false;
				break;
		}
		if (isinput)
			inputargno++;		/* this is a 1-based counter */

		if (print_table_args != (argmode == PROARGMODE_TABLE))
			continue;

		if (argsprinted == insertorderbyat)
		{
			if (argsprinted)
				appendStringInfoChar(buf, ' ');
			appendStringInfoString(buf, "ORDER BY ");
		}
		else if (argsprinted)
			appendStringInfoString(buf, ", ");

		appendStringInfoString(buf, modename);
		if (argname && argname[0])
			appendStringInfo(buf, "%s ", quote_identifier(argname));
		appendStringInfoString(buf, format_type_be(argtype));
		if (print_defaults && isinput && default_positions_available)
		{
			if (nextdefaultposition != NULL)
			{
				int position = intVal((Node *) lfirst(nextdefaultposition));
				Node *defexpr;

				Assert(nextargdefault != NULL);
				defexpr = (Node *) lfirst(nextargdefault);

				if (position == (inputargno - 1))
				{
					appendStringInfo(buf, " DEFAULT %s",
									 deparse_expression(defexpr, NIL, false, false));
					nextdefaultposition = lnext(defaultpositions, nextdefaultposition);
					nextargdefault = lnext(argdefaults, nextargdefault);
				}
			}
		}
		else if (print_defaults && isinput && inputargno > nlackdefaults)
		{
			Node	   *expr;

			Assert(nextargdefault != NULL);
			expr = (Node *) lfirst(nextargdefault);
			nextargdefault = lnext(argdefaults, nextargdefault);

			appendStringInfo(buf, " DEFAULT %s",
							 deparse_expression(expr, NIL, false, false));
		}
		argsprinted++;

		/* nasty hack: print the last arg twice for variadic ordered-set agg */
		if (argsprinted == insertorderbyat && i == numargs - 1)
		{
			i--;
			/* aggs shouldn't have defaults anyway, but just to be sure ... */
			print_defaults = false;
		}
	}

	if (default_positions_available)
		ReleaseSysCache(bbffunctuple);

	return argsprinted;
}
