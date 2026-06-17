---
description: STOP HOARDING. ethereal_read is default. Retain only architecture docs and key interfaces.
---
## STOP. You're hoarding context.

Every file you read_and_retain is injected into EVERY future turn. Your context window is finite.

### What you're doing wrong
- Reading files with read_and_retain by default
- Retaining files you only needed for one turn
- Not releasing files after the task is done

### What to do instead

1. **ethereal_read** for 90% of file reads. Set cycles=2 if you need it for the next turn.
2. **read_and_retain ONLY:**
   - Architecture documents (AGENTS.md, ARCHITECTURE.md)
   - Key config files you'll reference repeatedly
   - Project index / directory map
3. **After every 3 file reads:** run context_status. If >5 non-essential files retained → release the extras.
4. **When task is done:** release ALL retained files that aren't project-global reference docs.

### Tool decision tree
```
Need to read a file?
├─ For this turn only? → ethereal_read, cycles=1
├─ For next 2-3 turns? → ethereal_read, cycles=3  
├─ Permanent reference (config, architecture)? → read_and_retain
└─ Not sure? → ethereal_read (default conservative)
```

### Release command
```
read_and_retain(action="release", path="file/to/release")
```

Now go back and fix your context. Release the transient files. Keep only what earns its place in every turn.
