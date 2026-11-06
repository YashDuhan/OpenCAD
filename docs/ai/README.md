# OpenCAD's AI assistant

The AI sidebar (the sparkle button in the toolbar, open by default) talks to the model you choose in Providers
(OpenAI, Anthropic, Gemini, OpenRouter, a local or custom OpenAI-compatible server, and more) and works on the open
drawing with its tools. The code is in `librecad/src/ui/dock_widgets/ai_assistant/`.

## What the AI can use

- **Every toolbar button** (`list_toolbar_buttons`, `press_toolbar_button`, then `run_command`, `click_drawing`,
  `select_entities`, `tool_options`, `handle_dialog`; the toolbar lists with `set_toolbar_value`).
- **Menus** (`use_menu`), **panels and the status bar** (`use_panel`: Layers, Layers Tree, Blocks, Library, Pens,
  Named Views, UCS, Entity Info, the CAD tool panels...), **right-click menus** (`context_menu` on the drawing, and
  right-clicks on panel rows through `use_panel`), the **Properties panel** (`use_properties`), and anything else in
  the window by looking at it and clicking (`look_at_app`, `press_in_app`).
- **Drawing tools**: `capture_view`, `get_view`, zooming and panning, `get_drawing_info`, `list_entities`,
  `run_command`, `add_text`, `open_drawing`, `view_image_file`.
- **The drawing checker**: `check_drawing` lists problems (zero-length and tiny pieces, duplicates and overlaps,
  nearly closed polylines, gaps between ends, odd text heights, layer and style problems...), `fix_drawing_issues`
  applies the safe fixes as one undo step. Also from the shield button in the chat header (Check Drawing).
- **An in-app web browser** (`browser_navigate`, `browser_read`, `browser_click`, ...; never your system browser),
  **questions to you** (`request_clarification`), and the tools of connected **MCP servers**.

Which tools ask before running is set by the approval mode under the message box (Supervised, Auto-accept edits,
Auto, Full access) and per tool in Tool permissions; deleting, closing and writing files always ask unless you choose
Full access.

## Working with the AI

- **Talking**: Enter sends, Shift+Enter is a new line. The mic dictates (click, or hold Ctrl+Space with the panel
  focused; the chevron next to it chooses the speech engine: Gemini, OpenAI, Groq or a local whisper.cpp server).
- **Attachments**: drop, paste or pick (paperclip) PNG, JPEG, WebP and PDF files. Big images are scaled down; PDFs go
  to models that read them, or as page images to the others.
- **@-mentions**: type @ for @selection, @view, @drawing, a layer, a block or an entity: their details go with the
  question. "Ask AI about this" in the drawing's right-click menu starts a question about what you clicked.
- **Chat history per drawing**: every chat is saved and filed under its drawing; the clock button lists this
  drawing's chats or all of them (search, resume, rename, delete). Switching drawings shows that drawing's chat.
- **Plan first** (the checklist button): the AI proposes a plan you approve, edit or reject before it changes
  anything, and a checklist follows its progress.
- **Check & fix** under an answer runs a debugger pass that checks the drawing against your request and fixes what is
  wrong; "Check my work automatically" runs it after every run.
- **Review, Revert**: after a run that changed the drawing, its changes are highlighted (green: drawn, red dashes:
  deleted or replaced) with Accept and Reject; "Revert AI changes" under each answer takes a run back later (and
  Restore brings it back). Auto-review can be turned off in the review bar.
- **Take over**: while the AI works you can look around (zoom, pan, scroll); Take over pauses it and lets you work, it
  goes on after a few idle seconds and is told what you changed. Stop ends the run.

## Agent Progress

Agent Progress > Open Agent Progress... lists every toolbar button, menu entry, panel, status bar field, right-click
menu, Properties check, drawing preference and AI tool, whether the AI can use it and whether its test passed. Select
entries to test them on scratch drawings, or double-click one to give the AI a task with it.

## Tests

- **Every button, menu, panel and tool**: `scripts/ai/test_toolbar_buttons.sh` runs each with its recipe
  (`librecad/res/ai/toolbar_recipes.json`, plus the tests OpenCAD makes for menu entries, panels and the status bar)
  in the real app offscreen, and writes `docs/ai/toolbar-buttons-checklist.md`. Pass ids to run only some
  (`menu:*` runs every id starting with `menu:`).
- **The chat end to end**: `scripts/ai/e2e/run_all.sh` runs scripted conversations (`scripts/ai/e2e/scenarios/*`) in
  the real app with a fake model, dummy PDFs, images and audio; `REAL=gemini scripts/ai/e2e/run_all.sh` runs the
  `gemini_` scenarios against the Gemini key stored in your OpenCAD settings. The driver's steps are described at the
  top of `lc_aie2etest.cpp`.
