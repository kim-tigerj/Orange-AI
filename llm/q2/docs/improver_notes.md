# q2 Improver Notes

## 2026-05-16 run eaf896678852

Recent needs_review evidence was dominated by repeated stale zero-result cases:

- `파일 인벤토리에서 ProductVersion Signer HostUrl 조합별 파일 수를 보여줘 인벤토리 기준`
- `voidtools 제품을 버전별로 보여줘`

Current live verification does not reproduce those failures. The filelist field-distribution query now uses the template planner path that groups by `Signer`, `HostUrl`, and `ProductVersion` without literal metadata-name filters, and returns 100 rows. The `voidtools` installed-product query filters `nodeinfo` `UNINSTALL` by `data.publisher` and returns 4 version rows for Everything/voidtools.

No pending human improvement requests were present. The only current runtime error evidence is Mongo connectivity to `127.0.0.1:27018`, which is operational/environmental rather than a q2 planner/template defect.

Before changing planner or autotest code for this cluster, collect fresh failures after the current cache/template version is active. Useful evidence would be a new nonzero-probe `zero_result` case for a current-profile product or a repeated field-distribution query whose plan still contains literal filters such as `ProductVersion Signer HostUrl 조합별`.
