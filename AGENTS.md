# Repository Guidelines

## Project Structure & Module Organization

This workspace is currently empty: no application code, tests, assets, or build configuration has been added. Establish the layout with the first implementation and update this guide accordingly. Suggested directories are `src/` for application code, `tests/` for automated tests, and `assets/` for static resources. Keep modules focused on a single responsibility, and avoid committing generated downloads or build output.

## Build, Test, and Development Commands

No build, test, or local run commands are configured yet. When introducing a toolchain, document its prerequisites and exact install, run, build, and test commands in `README.md`. Prefer repository scripts over commands that depend on undocumented global tools.

For an initial workspace inspection in PowerShell, use `Get-ChildItem -Force`. Once Git is initialized, use `git status --short` to review pending changes.

## Coding Style & Naming Conventions

No language, formatter, or linter has been selected. Adopt the chosen language’s standard conventions and commit shared formatting and linting configuration with the first source files. Use descriptive names, consistent indentation, and small functions. Match existing module conventions as the codebase grows; avoid unrelated formatting changes.

## Testing Guidelines

No testing framework or coverage threshold exists yet. Add automated tests alongside new behavior and document how to run them. Follow the selected framework’s test naming conventions. For download functionality, cover successful transfers, interrupted requests, invalid inputs, and filesystem failures without relying on live external services.

## Commit & Pull Request Guidelines

No Git history is available to establish existing commit conventions. Use concise, imperative subjects such as `Add download queue validation`, and keep each commit focused.

Pull requests should explain the change, link relevant issues, and describe validation performed. Include screenshots for user-visible interface changes and identify any configuration or migration steps.

## Security & Configuration

Keep credentials, local configuration, temporary files, and downloaded content out of version control. Provide placeholder values in configuration examples. Validate download destinations and remote inputs before performing filesystem operations.
