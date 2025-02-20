drop table if exists babel_3291_t1
go

create table babel_3291_t1(a1 int PRIMARY KEY, b1 int)
go

select set_config('babelfishpg_tsql.explain_costs', 'off', false)
go

set babelfish_showplan_all on
go

/*
 * Run a SELECT query without any hints to ensure that un-hinted queries still work.
 * This also ensures that when the SELECT query is not hinted it produces a different plan(index scan)
 * than the sequential scan that we're hinting in the query below. This verifies that the next test is actually valid.
 * If the planner was going to choose a sequential scan anyway, the next test wouldn't actually prove that hints were working.
 */
select * from babel_3291_t1 where a1 = 1
go

/*
 * Run a SELECT query and give the hint to follow a sequential scan. 
 * The query plan should now use a sequential scan instead of the index scan it uses in the un-hinted test above.
 */
select /*+SeqScan(babel_3291_t1)*/ * from babel_3291_t1 where a1 = 1
go

set babelfish_showplan_all off
go

-- cleanup
select set_config('babelfishpg_tsql.explain_costs', 'on', false)
go

drop table babel_3291_t1
go