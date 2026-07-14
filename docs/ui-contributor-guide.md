# Inventatory UI Contributor Guide

## Structure

Page rendering and page-local input remain under `src/ui/pages`. Shared visual
roles and formatting belong under `src/ui/shared`. The application shell and
event dispatch live in `src/app.cpp`; cross-page workflows live in
`src/app/AppActions.cpp`.

## Adding an action

Register every command in `currentActions()`. Each action receives a stable ID,
label, group, visible key hint, trigger, and callback. Do not duplicate the same
accelerator in a page key switch. The action registry drives direct keyboard
dispatch and the clickable action sheet.

## Adding a clickable target

Wrap the rendered element with `App::target`. Give it a stable ID, semantic
target kind, activation callback, and accurate enabled state. Targets are
collected during each render and clicked in reverse visual order so overlays
win. Do not store secrets or inventory data in target IDs.

Keyboard focus uses the same targets. Every mouse action must retain a keyboard
route, and every accelerator must remain discoverable on screen or in Actions.

## Color and layout

Use semantic Instrument Blue helpers rather than literal RGB values. Blue is
for interaction/focus; semantic colors are for state. Use one surface for normal
rows and reserve hover/selection backgrounds for interaction. Prefer alignment
and a single dim divider over windows nested inside windows.

## Settings and secrets

Machine/user settings are versioned in `%LOCALAPPDATA%/Inventatory/settings.conf`.
Inventory content stays in the selected Inventatory data directory. Secrets use
`CredentialStore`; never write them to config, logs, activity history, test
snapshots, messages, or screenshots. New settings must define validation,
staging, save/cancel behavior, migration defaults, and runtime/restart effects.

## Verification

After changing terminal rendering, build, run the core tests, launch a fresh
agent-owned maximized Inventatory terminal, and inspect a window-only capture. Check
100x30, 120x30, and a wide layout for clipping, wrapping, focus, contrast,
scrolling, and action-sheet opacity.
