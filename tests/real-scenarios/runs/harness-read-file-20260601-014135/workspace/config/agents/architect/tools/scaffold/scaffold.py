#!/usr/bin/env python3
"""Scaffold generator — creates project structures and boilerplate."""
import sys
import json
import os
import re


def to_camel_case(snake_str):
    """Convert snake_case to CamelCase."""
    return ''.join(x.capitalize() for x in snake_str.split('_'))


def to_snake_case(camel_str):
    """Convert CamelCase to snake_case."""
    s1 = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', camel_str)
    return re.sub('([a-z0-9])([A-Z])', r'\1_\2', s1).lower()


def to_screaming_snake(snake_str):
    """Convert to SCREAMING_SNAKE_CASE."""
    return snake_str.upper()


def scaffold_cpp_class(name, path, options):
    """Generate C++ header and implementation files."""
    class_name = to_camel_case(name)
    guard = to_screaming_snake(name) + "_HPP"
    created = []

    hpp = f"""#pragma once

// =============================================================================
// {class_name}.hpp
// =============================================================================

#include <string>
#include <memory>

namespace cortex {{

class {class_name} {{
public:
    explicit {class_name}();
    ~{class_name}();

    // Non-copyable
    {class_name}(const {class_name}&) = delete;
    {class_name}& operator=(const {class_name}&) = delete;

    // Movable
    {class_name}({class_name}&&) noexcept = default;
    {class_name}& operator=({class_name}&&) noexcept = default;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
}};

}} // namespace cortex
"""

    cpp = f"""#include "{class_name}.hpp"

namespace cortex {{

struct {class_name}::Impl {{
    // Implementation details
}};

{class_name}::{class_name}()
    : impl_(std::make_unique<Impl>()) {{}}

{class_name}::~{class_name}() = default;

}} // namespace cortex
"""

    os.makedirs(path, exist_ok=True)
    hpp_path = os.path.join(path, f"{class_name}.hpp")
    with open(hpp_path, 'w') as f:
        f.write(hpp)
    created.append(hpp_path)

    cpp_path = os.path.join(path, f"{class_name}.cpp")
    with open(cpp_path, 'w') as f:
        f.write(cpp)
    created.append(cpp_path)

    return created


def scaffold_manifest_tool(name, path, options):
    """Generate a manifest tool module."""
    opts = json.loads(options) if isinstance(options, str) and options else {}
    runtime = opts.get("runtime", "python3")
    created = []

    tool_dir = os.path.join(path, name)
    os.makedirs(tool_dir, exist_ok=True)

    entrypoint = f"{name}.py" if runtime == "python3" else f"{name}.sh"

    yml = f"""kind: Tool
name: {name}
version: "1.0"
summary: "{name} tool"
description: |
  TODO: Add description
category: general
tags: [{name}]
input_schema:
  type: object
  required: []
  properties: {{}}
output_schema:
  type: object
  properties: {{}}
implementation:
  type: script
  entrypoint: {entrypoint}
  runtime: {runtime}
  timeout: 30
"""
    yml_path = os.path.join(tool_dir, "tool.yml")
    with open(yml_path, 'w') as f:
        f.write(yml)
    created.append(yml_path)

    if runtime == "python3":
        script = f"""#!/usr/bin/env python3
\"\"\"{name} tool.\"\"\"
import sys
import json

def run(params):
    # TODO: Implement
    return json.dumps({{"result": "not implemented", "params": params}})

if __name__ == "__main__":
    try:
        params = json.loads(sys.stdin.read())
    except json.JSONDecodeError:
        print(json.dumps({{"error": "Invalid JSON input"}}))
        sys.exit(1)
    print(run(params))
"""
        script_path = os.path.join(tool_dir, entrypoint)
        with open(script_path, 'w') as f:
            f.write(script)
        os.chmod(script_path, 0o755)
        created.append(script_path)

    return created


def scaffold_manifest_agent(name, path, options):
    """Generate a manifest agent module."""
    created = []

    agent_dir = os.path.join(path, name)
    os.makedirs(os.path.join(agent_dir, "system-prompts"), exist_ok=True)

    yml = f"""kind: Agent
name: {name}
version: "1.0"
summary: "{name} agent"
cognitive_engine:
  primary:
    provider: "zen"
    model: "big-pickle"
  parameters:
    temperature: 0.7
    max_tokens: 4096
persona:
  agent: "./system-prompts/{name}.md"
import:
  tools:
    - "exec"
    - "fs_read"
    - "fs_write"
    - "grep"
    - "list"
    - "json"
"""
    yml_path = os.path.join(agent_dir, "agent.yml")
    with open(yml_path, 'w') as f:
        f.write(yml)
    created.append(yml_path)

    prompt = f"""# {name.capitalize()}

You are a {name} agent.

TODO: Add detailed instructions.

## Behavior
- Use `<action>` tags to execute tools.
- Close with `<response final="true">` when done.

## Principles
- TODO: Add principles
"""
    prompt_path = os.path.join(agent_dir, "system-prompts", f"{name}.md")
    with open(prompt_path, 'w') as f:
        f.write(prompt)
    created.append(prompt_path)

    return created


def scaffold_manifest_relic(name, path, options):
    """Generate a manifest relic module with FastAPI service + Docker."""
    created = []

    relic_dir = os.path.join(path, name)
    os.makedirs(relic_dir, exist_ok=True)
    app_dir = os.path.join(relic_dir, "app")
    os.makedirs(app_dir, exist_ok=True)

    yml = f"""kind: Relic
name: {name}
version: "1.0"
summary: "{name} relic"
mode: managed
base_url: "http://localhost:8010"
endpoints:
  - name: list
    method: GET
    path: /{name}/list
  - name: get
    method: GET
    path: /{name}/{{{{key}}}}
  - name: create
    method: POST
    path: /{name}
"""
    yml_path = os.path.join(relic_dir, "relic.yml")
    with open(yml_path, 'w') as f:
        f.write(yml)
    created.append(yml_path)

    config_yml = f"""# {name} relic configuration
timeout: 10
retries: 2
access_control: none
"""
    config_path = os.path.join(relic_dir, "config.yml")
    with open(config_path, 'w') as f:
        f.write(config_yml)
    created.append(config_path)

    main_py = f"""#!/usr/bin/env python3
\"\"\"{name} relic — FastAPI service.\"\"\"
from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI(title="{name}", version="1.0")

# TODO: Define Pydantic models and endpoints

@app.get("/{name}/list")
async def list_items():
    return {{"items": [], "success": True}}

@app.get("/{name}/{{key}}")
async def get_item(key: str):
    return {{"key": key, "value": None, "success": True}}

@app.post("/{name}")
async def create_item():
    return {{"success": True}}
"""
    main_path = os.path.join(app_dir, "main.py")
    with open(main_path, 'w') as f:
        f.write(main_py)
    created.append(main_path)

    dockerfile = """FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app/ .
EXPOSE 8010
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8010"]
"""
    df_path = os.path.join(relic_dir, "Dockerfile")
    with open(df_path, 'w') as f:
        f.write(dockerfile)
    created.append(df_path)

    reqs = """fastapi>=0.100.0
uvicorn>=0.20.0
pydantic>=2.0.0
"""
    reqs_path = os.path.join(relic_dir, "requirements.txt")
    with open(reqs_path, 'w') as f:
        f.write(reqs)
    created.append(reqs_path)

    compose = f"""version: "3.8"
services:
  {name}:
    build: .
    ports:
      - "8010:8010"
    restart: unless-stopped
"""
    compose_path = os.path.join(relic_dir, "docker-compose.yml")
    with open(compose_path, 'w') as f:
        f.write(compose)
    created.append(compose_path)

    return created


def scaffold_manifest_workflow(name, path, options):
    """Generate a manifest workflow module."""
    created = []

    wf_dir = os.path.join(path, name)
    os.makedirs(wf_dir, exist_ok=True)

    yml = f"""kind: Workflow
name: {name}
version: "1.0"
summary: "{name} workflow"
description: |
  TODO: Add description

steps:
  - id: step_1
    agent: assistant
    prompt: "TODO: Add step prompt"
    depends_on: []

  - id: step_2
    agent: assistant
    prompt: "TODO: Add step prompt"
    depends_on: [step_1]

on_error: stop
output:
  format: json
"""
    yml_path = os.path.join(wf_dir, f"{name}.yml")
    with open(yml_path, 'w') as f:
        f.write(yml)
    created.append(yml_path)

    config_yml = f"""# {name} workflow configuration
timeout_per_step: 60
max_retries: 2
parallel: false
"""
    config_path = os.path.join(wf_dir, "config.yml")
    with open(config_path, 'w') as f:
        f.write(config_yml)
    created.append(config_path)

    return created


def scaffold_python_module(name, path, options):
    """Generate a Python module structure."""
    created = []

    mod_dir = os.path.join(path, name)
    os.makedirs(mod_dir, exist_ok=True)

    init_py = f'"""{name} module."""\n'
    init_path = os.path.join(mod_dir, "__init__.py")
    with open(init_path, 'w') as f:
        f.write(init_py)
    created.append(init_path)

    main_py = f'"""{name} main implementation."""\n\n\ndef run():\n    pass\n'
    main_path = os.path.join(mod_dir, f"{name}.py")
    with open(main_path, 'w') as f:
        f.write(main_py)
    created.append(main_path)

    return created


if __name__ == "__main__":
    try:
        params = json.loads(sys.stdin.read())
    except json.JSONDecodeError:
        print(json.dumps({"error": "Invalid JSON input"}))
        sys.exit(1)

    scaffold_type = params.get("type", "")
    name = params.get("name", "unnamed")
    path = params.get("path", ".")
    options = params.get("options", "{}")

    if not scaffold_type:
        print(json.dumps({"error": "type is required",
                          "available": ["cpp_class", "python_module", "manifest_tool",
                                        "manifest_agent", "manifest_relic", "manifest_workflow"]}))
        sys.exit(1)

    scaffolders = {
        "cpp_class": scaffold_cpp_class,
        "python_module": scaffold_python_module,
        "manifest_tool": scaffold_manifest_tool,
        "manifest_agent": scaffold_manifest_agent,
        "manifest_relic": scaffold_manifest_relic,
        "manifest_workflow": scaffold_manifest_workflow,
    }

    if scaffold_type not in scaffolders:
        print(json.dumps({"error": f"Unknown scaffold type: {scaffold_type}",
                          "available": list(scaffolders.keys())}))
        sys.exit(1)

    try:
        created = scaffolders[scaffold_type](name, path, options)
        print(json.dumps({
            "created": created,
            "count": len(created),
            "type": scaffold_type,
            "name": name
        }, indent=2))
    except Exception as e:
        print(json.dumps({"error": str(e)}))
