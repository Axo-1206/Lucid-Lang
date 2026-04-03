---
name: task-complete
description: "Complete task and log it. Syntax: /task-complete id=\"1\". Removes task from Tasks table and adds its description to Log with today's date."
---

When a task is completed:
1. Remove the task row from the Tasks table in `./context/TASK_LOG.md` by matching its ID
2. Add an entry to the Log table with:
   - Current date ({{CURRENT_DATE}}) - completion date, not original task date
   - Task description from the completed task

Format log entry as: `| {{CURRENT_DATE}} | [task description] |`

# Example usage:
# /task-complete id="1"
# /task-complete id="3"

Parameters:
- id: The numeric ID of the task to complete and remove