# First-product review: Cortex on inkcell (beta)

**Audience:** post-compaction agents + operator.  
**Focus:** implementation ergonomics + UX of *integration*, not migration status (cutover is done).

## Verdict

Cortex uses inkcell as **retained Surface + Engine**, then rebuilds much of an app framework in product headers. Valid beta survival. Not portfolio-grade integration yet either direction.

## Best

- Domain kept out of inkcell  
- wake_fd + 33ms tick coalesce (canonical product recipe — should live in inkcell COOKBOOK)  
- Offline UI/perf gates + snapshots  
- Partial TextArea/ScrollView **state** adoption  
- Product pressure fixed engine Ctrl-C ordering  

## Worst

1. ShellModel god object (~1.7k LOC header)  
2. Reinvented shell/theme/focus/commands  
3. Stringly `pendingRoute` + global `g_running`  
4. Dual TUI cognitive load  
5. Mega-scenes as key routers  
6. Custom transcript paint (virtualization never improves library ScrollView)  
7. Parallel theme system  
8. Header-only UI compile tax  

## Scorecard

| Dimension | Grade |
|-----------|-------|
| Domain isolation | A- |
| Engine usage | B+ |
| Widget dogfood | D+ |
| Theme dogfood | D |
| Focus/commands/routes | D |
| Scene modularity | C- |
| Perf discipline | B |
| Dual-stack clarity | C |

## Dual-track rule

Every PR should answer: **dogfood inkcell API** or **add missing API upstream**?  
“Third product-local framework” is the failure mode.

See `docs/INKCELL_INTEGRATION.md` and `docs/tui-qol/README.md`.
