



## Tools
## Relic

## Manifest brainstorming and idea dumps

We need some sort of:
- ssh/remoye bash/fs tool and or relic
- we need to create all the runtime builtins (aka, runtime and user features that we can give access for the agent): reload (hotreload access) ,  this should include all sorts of getters and setters as well for agent/manifest attributes/modifiers as well, expose compacting and context related methods from the runtime (of course not most of these will be used in an agent, its possible, but they are meant to be used for special agents, I dont get to decide that, as it is in user/manifest-dev scope),

QOL & Additions:
- a flag in the agent manifest to make itself available in the subagent list.
- file read/write tool QOL: in select mode, clicking on the block will copy the contents to the clipboard, and in edit mode, clicking on the block will open a modal with a text area to edit the contents of the file or maybe highlight a part in vmode and ask the agent about it.
<!-- - in TUI select mode, clicking on a block will copy the contents to the clipboard -->
- in TUI select mode, I need G and gg for navigation and I should probably make the select mode start from latest instead of oldest.
- in general, start brainstorming for 'what to do' for each block pressed in select mode, for now just for anything builtin/native.
- a /continue
- start working on the workflow manifest renderer and finalize v1 runtime.
- select mode should handle the "newest block case" better, 
- prompt box still needs a lot of work, at very least shift-enter, and scrolling back to edit ...
- each session gets names and notes now, editable via pop left sidebar that has session metadata or from the hub/main menu in the session list.
- chat will also have a left sidebar pop up with a compact chat view along with some metrics and useful data/features.
- a way to link to other tui binaries inside of the harness be able the switch (with options like preserve: true|false, ...) since Ill probably have the harness/tui open 24/7 on my second screen.
- could also have terminal/process management in the TUI/harness chat later on.
- we need a size limit cap option asap
- filter: ... to filter off tags from context, lazy thought i think this relates to other things.
- files/folder binding for docker and normal sandboxes.
- CHAT QOL, tell user and LLM when turns/cycle/... exhaust, simply inject a tag
- need a way to manage live/active session and navigation, fork new session normal and ephemereal(this will probally get used a lot, by default it give a /ask or whatnot but you can do anything with it) from chat page among other utility and QOL I want there
- we also need a useful sidebar or even 2 for the chat page
- add throtleling (requests per second and or tokens per second among other modifiers to add)
- as well as start working on the selection ops: main op defaults to enter the rest can have custom binds, they can even be repeated across types since they dont overlap(eg. o-O for open and open <modifier>(depending on what the highlighted block is)):
    - read mode (full page where we can read navigate and use V mode, select and ask, rendered markdown ...)
    - more keybinds and ops for the subagent, 
    - 

- selection only locks when its highlighting the last block/action else it can move freely, we should also add gg/G to it like come on, maybe even ctrl-o/i later on (not as important as the rest IMO)
- start working on inkcell/examples/asset-editor or whatnot, full asset generation and a specialized agent on top. rought impromptu idea here, but we will need a hub/editor like with all engines.
    - then I want to create a cycling wheel component/widget/module with full and smooth animations, ... like in gta 5, rust, and a lot of games. its a circular selection wheel.  
    - 
- need to add hooks: and rules: to agent manifest.
