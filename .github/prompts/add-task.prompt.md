---
name: task-add
description: "Add new task to TASK_LOG.md. Syntax: /task-add name=\"Task name\" description=\"Task description\". ID auto-increments based on task count."
---

Add a new row to the Tasks table in `./context/TASK_LOG.md` with:
- Current date ({{CURRENT_DATE}})
- Auto-incremented ID (based on number of existing tasks)
- Task name and description (provided by user)

Format: `| {{CURRENT_DATE}} | [auto-ID] | [name] | [description] |`

# Example usage:
# /task-add name="Implement type checking" description="Add type checking to semantic analysis"
# /task-add name="Optimize parser" description="Improve parser performance for large files"

Parameters:
- name: The task name
- description: Detailed description of the task