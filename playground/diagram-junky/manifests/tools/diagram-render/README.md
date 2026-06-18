# diagram_render tool

Project-local MK3 tool wrapper around `playground/diagram-junky/render.py`.

## Purpose

Give agents a narrow, manifest-importable interface for diagram-junky work:

- render examples or diagram files
- inspect metadata and bounds
- validate against `diagram-document.schema.json`
- list examples and renderer styles
- write rendered output to a file

## Example action

```xml
<action type="tool" name="diagram_render" id="dr1" mode="sync">
{"action":"render","example":"ansi-showcase","width":120,"height":36,"theme":"neon","color":"never","legend":true}
</action>
```

The script returns a JSON object with `success`, `action`, `command`, and `output`.
