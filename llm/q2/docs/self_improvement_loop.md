# q2 Self-Improvement Loop

q2 does not rewrite production prompts or templates by itself. It records evidence, runs bounded probes, and stores improvement candidates that can be reviewed and promoted into code, prompt, or eval changes.

## Collections

- `ai.llm_query`: every natural-language request, generated plan, aggregation, execution result, errors, and quality fields.
- `ai.llm_improvement_candidate`: proposed improvements derived from failed or suspicious requests.
- `ai.autotest_status`: live autonomous test progress and latest summary.

If the configured Mongo user cannot write to `ai`, q2 writes the same records to MongoDB fallback collections in `public` with a `q2_` prefix, such as `public.q2_llm_query`. Runtime data must remain in MongoDB, not local files.

## Quality Status

- `ok`: query executed and returned at least one row.
- `unknown`: plan-only response or result quality cannot be determined.
- `failed`: planner/runtime failure.
- `needs_review`: query executed but returned zero rows.

## Failure Reasons

- `planner_failed`: no valid `QueryPlan`.
- `mongo_connectivity`: MongoDB/tunnel unavailable.
- `mongo_timeout`: MongoDB execution timeout.
- `runtime_error`: query execution failed for another reason.
- `zero_result`: query executed successfully but returned no rows.

## Zero-Result Probes

For `zero_result`, q2 tries small, bounded alternatives:

- `time_expand_365d`: if the request has a short/default relative range, retry the same plan over 365 days.
- `collection_detect/report/timeline`: for event-style collections, test sibling event collections.
- `filter_field_*`: for product/company terms, test likely text fields such as `CompanyName`, `ProductName`, `FileDescription`, `FileName`, `FilePath`, `Name`, and `Desc`.

Only a small sample is fetched. Probe results are evidence, not an automatic production change.

## Candidate Fix Types

- `default_time_range`: widen default time range for a class of questions.
- `explicit_time_window_no_data`: keep the requested time window, but note that older data exists in a wider probe.
- `collection_routing`: route a natural-language intent to a better collection.
- `filter_field_mapping`: map a product/company/user term to a better field.
- `template_candidate`: add or adjust a deterministic template.
- `runtime_failure`: fix infrastructure, timeout, or aggregation behavior.
- `needs_human_review`: no probe found a better path.

## Promotion Rule

A candidate becomes production behavior only after:

1. Add or update an eval case.
2. Implement the smallest template/prompt/code patch.
3. Run q2 eval.
4. Restart q2 service.

This keeps the loop autonomous in evidence gathering while keeping runtime behavior controlled.
