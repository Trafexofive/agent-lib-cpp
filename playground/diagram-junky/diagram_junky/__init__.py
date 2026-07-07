"""Reusable diagram-junky core: renderer + workspace client."""

from .client import EventStream, FileWatch, StreamEvent, WorkspaceClient, WorkspaceError
from .rendering import Renderer, diagram_bounds, fit_viewport, load_doc, render_once

__all__ = [
    "Renderer",
    "diagram_bounds",
    "fit_viewport",
    "load_doc",
    "render_once",
    "EventStream",
    "FileWatch",
    "StreamEvent",
    "WorkspaceClient",
    "WorkspaceError",
]
